package bridge

import (
	"bufio"
	"bytes"
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

// Targets 只读部署映射: Star 内部数值端点 -> 独立 /metrics URL, 不单独限制节点数量.
// 本地配置总字节限制为 8 MiB, 足够覆盖准入目录; 抓取范围仍由可信目录决定.
// 不使用浏览器传入的 URL、不猜测相邻端口、不从环境变量启用代理.
// path 为本地映射文件路径, 为空表示关闭抓取; 返回端点到 URL 的映射, 文件非法时返回错误.
func Targets(path string) (map[string]string, error) {
	result := make(map[string]string)
	if path == "" {
		// 未配置文件路径即关闭指标抓取, 返回空映射而非错误.
		return result, nil
	}
	file, err := os.Open(path)
	if err != nil {
		return nil, errors.New("cannot open metrics targets")
	}
	defer file.Close()
	// 限制读取上限, 超限文件直接拒绝, 不把超大配置载入内存.
	const maximum = 8 << 20
	data, err := io.ReadAll(io.LimitReader(file, maximum+1))
	if err != nil || len(data) > maximum {
		return nil, errors.New("metrics targets exceed limit")
	}
	// 流式解析顶层对象, 必须是 { 开头, 不接受数组或其他形状.
	decoder := json.NewDecoder(bytes.NewReader(data)) // 直接借用本次拥有的响应, 不先复制为完整字符串.
	if token, err := decoder.Token(); err != nil || token != json.Delim('{') {
		return nil, errors.New("metrics targets must be an object")
	}
	// 逐项校验键值: 键为规范 Star 端点, 值为严格 /metrics URL, 重复键直接拒绝.
	for decoder.More() {
		token, err := decoder.Token()
		key, valid := token.(string)
		var value string
		if err != nil || !valid || decoder.Decode(&value) != nil || result[key] != "" {
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
	// 对象必须以 } 正常结束, 且其后不允许尾随数据.
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
	ID        string            `json:"id"`        // 被观测 Star 的实例 ID, 实例更替时重置本条记录.
	Observed  time.Time         `json:"observed"`  // 最近一次成功抓取的时间, 判断陈旧的依据.
	Attempted time.Time         `json:"attempted"` // 最近一次抓取尝试的时间 (无论成功失败).
	Stale     bool              `json:"stale"`     // 是否陈旧 (失败、过期或实例更替后置位).
	Values    map[string]string `json:"values"`    // 指标名到原始文本值, 发布后不可变.
}

// gauges 为 Star 当前实际实现的八个无标签整数状态, 不把不存在的直方图伪造为零.
// 值为 true 表示二值量 (只接受 0/1), false 表示普通计数器.
var gauges = map[string]bool{
	"astra_ready": true, "astra_almanac_ready": true, "astra_clock_ready": true, "astra_clock_synchronized": true,
	"astra_clock_uncertainty_nanoseconds": false, "astra_members": false, "astra_sessions": false, "astra_recovery_bytes": false,
}

// parseMetrics 只提取既有 Star 契约, 全部必需项合法且无重复才发布; 不解析任意标签表达式.
// data 为单次抓取的响应正文; 返回指标名到原始文本值, 缺项/重复/非法时返回错误.
func parseMetrics(data []byte) (map[string]string, error) {
	values := make(map[string]string, len(gauges))
	scanner := bufio.NewScanner(bytes.NewReader(data)) // Scanner.Text 为保留的字段提供独立字符串, 不让结果借用输入缓存.
	// 逐行解析: 空行与注释跳过, 未知指标跳过, 已知指标必须恰好两列且不重复.
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
		// 数值必须为规范十进制 uint64, 二值量只接受 0/1, 文本与数值回写必须一致 (禁前导零等变体).
		value, err := strconv.ParseUint(fields[1], 10, 64)
		if err != nil || binary && value > 1 || strconv.FormatUint(value, 10) != fields[1] {
			return nil, errors.New("invalid metrics value")
		}
		values[fields[0]] = fields[1]
	}
	// 扫描错误或缺项都视为不完整样本, 不发布部分指标.
	if scanner.Err() != nil || len(values) != len(gauges) {
		return nil, errors.New("incomplete metrics sample")
	}
	return values, nil
}

// Monitor 单一后台所有者, 四个工作者、两秒单请求、64 KiB 响应; 父 Context 取消后等待全部退出.
// 客户端不跟随重定向, 不向指标端点发送节点 bearer 或 Cookie. HTTPS 使用部署 CA.
// ctx 为父上下文, 取消即停止轮询; targets 为部署映射; certificate 为抓取用 TLS 配置.
func (backend *Backend) Monitor(ctx context.Context, targets map[string]string, certificate *tls.Config) {
	if len(targets) == 0 {
		return
	} // 默认关闭时不创建空轮询工作者或无意义计时器.
	// 传输层固定: 禁代理、两秒拨号/响应头超时、单主机单连接、空闲连接保留 ten 秒复用.
	transport := &http.Transport{Proxy: nil, TLSClientConfig: certificate.Clone(), DialContext: (&net.Dialer{Timeout: 2 * time.Second}).DialContext, MaxConnsPerHost: 1, MaxIdleConns: len(targets), MaxIdleConnsPerHost: 1, IdleConnTimeout: 10 * time.Second, ResponseHeaderTimeout: 2 * time.Second, MaxResponseHeaderBytes: 8192, DisableCompression: true}
	defer transport.CloseIdleConnections()
	client := &http.Client{Transport: transport, Timeout: 2 * time.Second, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}
	// 每五秒全量采样一轮, 退出时停止计时器, 不留后台 goroutine.
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
// client 为复用的抓取客户端; targets 为部署映射, 只抓取目录与映射交集.
func (backend *Backend) sample(ctx context.Context, client *http.Client, targets map[string]string) {
	// 先捕获不可变目录投影, 抓取全程不持目标锁, 不阻塞目录刷新.
	backend.mutex.RLock()
	nodes := backend.nodes
	backend.mutex.RUnlock()
	jobs := make(chan node)
	var group sync.WaitGroup
	defer func() { close(jobs); group.Wait() }() // 正常完成和取消共用一个退出责任, 先停输入再等待真实工作者退出.
	// 四个工作者并发抓取, 结果回写时校验实例是否已被目录更替, 更替则丢弃.
	for range 4 {
		group.Go(func() {
			for current := range jobs {
				values, err := scrape(ctx, client, targets[current.Endpoint], current.ID)
				backend.mutex.Lock()
				valid := backend.members[current.Endpoint] == current.ID
				if valid && !backend.closed {
					if backend.metrics == nil {
						backend.metrics = make(map[string]metric)
					}
					previous := backend.metrics[current.Endpoint]
					if previous.ID != current.ID {
						// 实例已更替, 丢弃旧观测, 从陈旧状态重新开始.
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
	// 只向 Star 角色且在部署映射中的端点派发任务, 取消时直接返回, 由 defer 负责收尾.
	for _, current := range nodes {
		if current.Role != "ROLE_STAR" || targets[current.Endpoint] == "" {
			continue
		}
		select {
		case jobs <- current:
		case <-ctx.Done():
			return
		}
	}
}

// scrape 成功必须同时匹配可信实例头、状态、大小和完整指标集; 网络失败只标陈旧, 不删除节点.
// target 为抓取 URL, instance 为目录期望的实例 ID; 返回解析后的指标文本值.
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
	// 状态码与实例头双重校验, 防止抓到代理错误页或已更替实例的残留响应.
	if response.StatusCode != http.StatusOK || len(response.Header.Values("X-Astra-Instance")) != 1 || response.Header.Get("X-Astra-Instance") != instance {
		return nil, errors.New("metrics identity unavailable")
	}
	// 响应正文限 64 KiB, 超限直接拒绝, 不把超大正文载入解析.
	data, err := io.ReadAll(io.LimitReader(response.Body, 65537))
	if err != nil || len(data) > 65536 {
		return nil, errors.New("metrics body exceeds limit")
	}
	return parseMetrics(data)
}

// Metrics 覆盖当前目录的全部 Star, 不由浏览器刷新发起网络请求.
// 未抓取/替换后的实例返回零观察时间和空值, 失败和过期观察明确标为 stale, 不隐去未采集节点.
func (backend *Backend) Metrics() any {
	backend.mutex.RLock()
	defer backend.mutex.RUnlock()
	values := make([]metric, 0, len(backend.nodes))
	now := time.Now()
	for _, current := range backend.nodes {
		if current.Role != "ROLE_STAR" {
			continue
		} // 其他角色尚未提供相同指标契约, 不虚构 Star 业务指标.
		value, exists := backend.metrics[current.Endpoint]
		if !exists || value.ID != current.ID {
			value = metric{ID: current.ID, Stale: true}
		}
		value.Stale = value.Stale || backend.closed || now.Sub(value.Observed) > 15*time.Second
		value.Observed, value.Attempted = value.Observed.UTC(), value.Attempted.UTC()
		values = append(values, value) // Values 发布后不可变, 不为每次请求复制 map.
	}
	return struct {
		Samples []metric `json:"samples"`
	}{values}
}
