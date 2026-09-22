package bridge

import (
	"bufio"
	"context"
	"crypto/tls"
	"encoding/json"
	"errors"
	"io"
	"net"
	"net/http"
	"net/url"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/eosforge/verdandi/astra/internal/admission"
)

// Targets 只读部署映射: Star 内部数值端点 -> 独立 /metrics URL, 最多 64 项.
// 不使用浏览器传入的 URL、不猜测相邻端口、不从环境变量启用代理.
func Targets(path string) (map[string]string, error) {
	result := make(map[string]string)
	if path == "" {
		return result, nil
	}
	file, err := os.Open(path)
	if err != nil {
		return nil, errors.New("cannot open metrics targets")
	}
	defer file.Close()
	data, err := io.ReadAll(io.LimitReader(file, 65537))
	if err != nil || len(data) > 65536 {
		return nil, errors.New("metrics targets exceed limit")
	}
	decoder := json.NewDecoder(strings.NewReader(string(data)))
	if token, err := decoder.Token(); err != nil || token != json.Delim('{') {
		return nil, errors.New("metrics targets must be an object")
	}
	for decoder.More() {
		token, err := decoder.Token()
		key, valid := token.(string)
		var value string
		if err != nil || !valid || decoder.Decode(&value) != nil || len(result) >= 64 || result[key] != "" {
			return nil, errors.New("invalid or duplicate metrics target")
		}
		if _, err := admission.Endpoint(key); err != nil {
			return nil, errors.New("metrics target requires a canonical Star endpoint")
		}
		parsed, err := url.Parse(value)
		if err != nil || (parsed.Scheme != "http" && parsed.Scheme != "https") || parsed.Path != "/metrics" || parsed.RawPath != "" || parsed.RawQuery != "" || parsed.ForceQuery || parsed.Fragment != "" || parsed.User != nil || parsed.Opaque != "" {
			return nil, errors.New("invalid metrics URL")
		}
		if _, err := admission.Endpoint(parsed.Host); err != nil {
			return nil, errors.New("metrics URL requires a canonical IP:PORT")
		}
		result[key] = value
	}
	if token, err := decoder.Token(); err != nil || token != json.Delim('}') {
		return nil, errors.New("invalid metrics object end")
	}
	if _, err := decoder.Token(); err != io.EOF {
		return nil, errors.New("trailing metrics target data")
	}
	return result, nil
}

// metric 是脱敏、固定基数的最近一次观测, 数字以字符串保存, 不损失 uint64 精度.
type metric struct {
	ID        string            `json:"id"`
	Observed  time.Time         `json:"observed"`
	Attempted time.Time         `json:"attempted"`
	Stale     bool              `json:"stale"`
	Values    map[string]string `json:"values"`
}

// gauges 为 Star 当前实际实现的八个无标签整数状态, 不把不存在的直方图伪造为零.
var gauges = map[string]bool{
	"astra_ready": true, "astra_almanac_ready": true, "astra_clock_ready": true, "astra_clock_synchronized": true,
	"astra_clock_uncertainty_nanoseconds": false, "astra_members": false, "astra_sessions": false, "astra_recovery_bytes": false,
}

// parseMetrics 只提取既有 Star 契约, 全部必需项合法且无重复才发布; 不解析任意标签表达式.
func parseMetrics(data []byte) (map[string]string, error) {
	values := make(map[string]string, len(gauges))
	scanner := bufio.NewScanner(strings.NewReader(string(data)))
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		fields := strings.Fields(line)
		if len(fields) == 0 {
			continue
		}
		binary, known := gauges[fields[0]]
		if !known {
			continue
		}
		if len(fields) != 2 || values[fields[0]] != "" {
			return nil, errors.New("invalid metrics sample")
		}
		value, err := strconv.ParseUint(fields[1], 10, 64)
		if err != nil || binary && value > 1 || strconv.FormatUint(value, 10) != fields[1] {
			return nil, errors.New("invalid metrics value")
		}
		values[fields[0]] = fields[1]
	}
	if scanner.Err() != nil || len(values) != len(gauges) {
		return nil, errors.New("incomplete metrics sample")
	}
	return values, nil
}

// Monitor 单一后台所有者, 四个工作者、两秒单请求、64 KiB 响应; 父 Context 取消后等待全部退出.
// 客户端不跟随重定向, 不向指标端点发送节点 bearer 或 Cookie. HTTPS 使用部署 CA.
func (backend *Backend) Monitor(ctx context.Context, targets map[string]string, certificate *tls.Config) {
	if len(targets) == 0 {
		return
	} // 默认关闭时不创建空轮询工作者或无意义计时器.
	transport := &http.Transport{Proxy: nil, TLSClientConfig: certificate.Clone(), DialContext: (&net.Dialer{Timeout: 2 * time.Second}).DialContext, MaxConnsPerHost: 1, MaxIdleConns: 4, MaxIdleConnsPerHost: 1, IdleConnTimeout: 10 * time.Second, ResponseHeaderTimeout: 2 * time.Second, MaxResponseHeaderBytes: 8192, DisableCompression: true}
	defer transport.CloseIdleConnections()
	client := &http.Client{Transport: transport, Timeout: 2 * time.Second, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}
	ticker := time.NewTicker(5 * time.Second)
	defer ticker.Stop()
	for {
		backend.sample(ctx, client, targets)
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
		}
	}
}

// sample 捕获目录投影后在锁外抓取; 目录中的旧实例更替时丢弃迟到结果.
func (backend *Backend) sample(ctx context.Context, client *http.Client, targets map[string]string) {
	backend.mutex.RLock()
	nodes := backend.nodes
	backend.mutex.RUnlock()
	jobs := make(chan node)
	var group sync.WaitGroup
	for range 4 {
		group.Go(func() {
			for current := range jobs {
				values, err := scrape(ctx, client, targets[current.Endpoint], current.ID)
				backend.mutex.Lock()
				valid := false
				for _, newest := range backend.nodes {
					if newest.ID == current.ID && newest.Endpoint == current.Endpoint {
						valid = true
						break
					}
				}
				if valid && !backend.closed {
					if backend.metrics == nil {
						backend.metrics = make(map[string]metric)
					}
					previous := backend.metrics[current.Endpoint]
					if previous.ID != current.ID {
						previous = metric{ID: current.ID, Stale: true}
					}
					// 缓存保留单调分量, UTC 只用于响应副本; 提前转换会让过期判断重新受墙钟跳变影响.
					previous.Attempted, previous.Stale = time.Now(), err != nil
					if err == nil {
						previous.Values, previous.Observed = values, previous.Attempted
					}
					backend.metrics[current.Endpoint] = previous
				}
				backend.mutex.Unlock()
			}
		})
	}
	for _, current := range nodes {
		if current.Role != "ROLE_STAR" || targets[current.Endpoint] == "" {
			continue
		}
		select {
		case jobs <- current:
		case <-ctx.Done():
			close(jobs)
			group.Wait()
			return
		}
	}
	close(jobs)
	group.Wait()
}

// scrape 成功必须同时匹配可信实例头、状态、大小和完整指标集; 网络失败只标陈旧, 不删除节点.
func scrape(ctx context.Context, client *http.Client, target, instance string) (map[string]string, error) {
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, target, nil)
	if err != nil {
		return nil, err
	}
	response, err := client.Do(request)
	if err != nil {
		return nil, err
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK || len(response.Header.Values("X-Astra-Instance")) != 1 || response.Header.Get("X-Astra-Instance") != instance {
		return nil, errors.New("metrics identity unavailable")
	}
	data, err := io.ReadAll(io.LimitReader(response.Body, 65537))
	if err != nil || len(data) > 65536 {
		return nil, errors.New("metrics body exceeds limit")
	}
	return parseMetrics(data)
}

// Metrics 只读取缓存, 不由浏览器刷新发起网络请求. 未抓取、失败和过期观察明确标为 stale.
func (backend *Backend) Metrics() any {
	backend.mutex.RLock()
	defer backend.mutex.RUnlock()
	values := make([]metric, 0, len(backend.metrics))
	now := time.Now()
	for _, current := range backend.nodes {
		value, exists := backend.metrics[current.Endpoint]
		if !exists || value.ID != current.ID {
			continue
		}
		value.Stale = value.Stale || backend.closed || now.Sub(value.Observed) > 15*time.Second
		value.Observed, value.Attempted = value.Observed.UTC(), value.Attempted.UTC()
		values = append(values, value) // Values 发布后不可变, 不为每次请求复制 map.
	}
	return struct {
		Samples []metric `json:"samples"`
	}{values}
}
