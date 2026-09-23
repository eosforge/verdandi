package bridge

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// complete 使用当前固定契约构造公开测试样本, 不产生业务内容或凭据.
func complete() string {
	var result strings.Builder
	for name := range gauges {
		fmt.Fprintf(&result, "# TYPE %s gauge\n%s 1\n", name, name)
	}
	return result.String()
}

func TestMetricParsing(t *testing.T) {
	if values, err := parseMetrics([]byte(complete())); err != nil || len(values) != 8 {
		t.Fatal("complete sample rejected")
	}
	for _, body := range []string{"", complete() + "astra_ready 1\n", strings.Replace(complete(), "astra_ready 1", "astra_ready 2", 1), strings.Replace(complete(), "astra_members 1", "astra_members NaN", 1), strings.Replace(complete(), "astra_members 1", "astra_members 18446744073709551616", 1)} {
		if _, err := parseMetrics([]byte(body)); err == nil {
			t.Fatal("invalid sample accepted")
		}
	}
}

func TestMetricTargets(t *testing.T) {
	path := filepath.Join(t.TempDir(), "metrics.json")
	for _, body := range []string{`[]`, `{"127.0.0.1:1":"http://127.0.0.1:2/metrics","127.0.0.1:1":"http://127.0.0.1:3/metrics"}`, `{"127.0.0.1:1":"http://localhost:2/metrics"}`, `{"127.0.0.1:1":"http://user:pass@127.0.0.1:2/metrics"}`, `{"127.0.0.1:1":"http://127.0.0.1:2/metrics?secret=x"}`, `{} {}`} {
		if err := os.WriteFile(path, []byte(body), 0600); err != nil {
			t.Fatal(err)
		}
		if _, err := Targets(path); err == nil {
			t.Fatal("invalid deployment targets accepted")
		}
	}
	if err := os.WriteFile(path, []byte(`{"127.0.0.1:1":"http://127.0.0.1:2/metrics"}`), 0600); err != nil {
		t.Fatal(err)
	}
	if values, err := Targets(path); err != nil || len(values) != 1 {
		t.Fatal("valid deployment rejected")
	}
}

// 真实回环 HTTP 验证身份、大小与部分正文拒绝, 不启动外部服务.
func TestMetricScrape(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(response http.ResponseWriter, request *http.Request) {
		response.Header().Set("X-Astra-Instance", "current")
		switch request.URL.Path {
		case "/large":
			_, _ = response.Write([]byte(strings.Repeat("x", 65537)))
		case "/partial":
			_, _ = response.Write([]byte("astra_ready 1\n"))
		default:
			_, _ = response.Write([]byte(complete()))
		}
	}))
	defer server.Close()
	client := &http.Client{Timeout: time.Second}
	if _, err := scrape(context.Background(), client, server.URL, "current"); err != nil {
		t.Fatal("valid scrape failed", err)
	}
	for _, value := range []struct{ path, id string }{{"", "old"}, {"/large", "current"}, {"/partial", "current"}} {
		if _, err := scrape(context.Background(), client, server.URL+value.path, value.id); err == nil {
			t.Fatal("untrusted or incomplete observation accepted")
		}
	}
	backend := &Backend{nodes: []node{{ID: "current", Role: "ROLE_STAR", Endpoint: "endpoint"}}, metrics: map[string]metric{"endpoint": {ID: "old", Observed: time.Now()}}}
	if value := fmt.Sprint(backend.Metrics()); strings.Contains(value, "old") {
		t.Fatal("old identity observation leaked into replacement")
	}
}

// 部署文件和实际采样都覆盖完整目录, 第 65 个节点不能被配置上限或返回投影截断.
func TestMetricDirectory(t *testing.T) {
	const count = 128
	targets := make(map[string]string, count)
	for index := range count {
		targets[fmt.Sprintf("127.0.0.1:%d", 10000+index)] = fmt.Sprintf("http://127.0.0.1:%d/metrics", 20000+index)
	}
	data, err := json.Marshal(targets)
	if err != nil {
		t.Fatal(err)
	}
	// 大目录允许正常格式化空白, 不再沿用单端响应的 64 KiB 配置限制.
	data = append([]byte(strings.Repeat(" ", 65536)), data...)
	path := filepath.Join(t.TempDir(), "metrics.json")
	if err := os.WriteFile(path, data, 0600); err != nil {
		t.Fatal(err)
	}
	if loaded, err := Targets(path); err != nil || len(loaded) != count {
		t.Fatal("deployment truncated the directory", err)
	}

	// 一个本地 HTTP 夹具模拟不同实例; 这里只核验 sample 覆盖范围, 不替代部署 URL 校验.
	server := httptest.NewServer(http.HandlerFunc(func(response http.ResponseWriter, request *http.Request) {
		response.Header().Set("X-Astra-Instance", strings.TrimPrefix(request.URL.Path, "/"))
		_, _ = response.Write([]byte(complete()))
	}))
	defer server.Close()
	backend := &Backend{members: make(map[string]string, count)}
	for index := range count {
		endpoint := fmt.Sprintf("127.0.0.1:%d", 10000+index)
		id := fmt.Sprintf("star-%d", index)
		backend.nodes = append(backend.nodes, node{ID: id, Role: "ROLE_STAR", Endpoint: endpoint})
		backend.members[endpoint] = id
		targets[endpoint] = server.URL + "/" + id
	}
	backend.nodes = append(backend.nodes, node{ID: "unconfigured", Role: "ROLE_STAR", Endpoint: "127.0.0.1:30000"}, node{ID: "polaris", Role: "ROLE_POLARIS"})
	backend.sample(t.Context(), server.Client(), targets)
	if len(backend.metrics) != count {
		t.Fatal("not all configured Stars were sampled")
	}
	encoded, err := json.Marshal(backend.Metrics())
	if err != nil {
		t.Fatal(err)
	}
	var output struct {
		Samples []metric `json:"samples"`
	}
	if err := json.Unmarshal(encoded, &output); err != nil || len(output.Samples) != count+1 {
		t.Fatal("metrics projection omitted part of the Star directory", err)
	}
	for _, value := range output.Samples {
		if value.ID == "unconfigured" {
			if !value.Stale || !value.Observed.IsZero() || len(value.Values) != 0 {
				t.Fatal("unconfigured Star claimed a successful observation")
			}
		} else if value.Stale || value.Observed.IsZero() || len(value.Values) != len(gauges) {
			t.Fatal("configured Star lacks its complete observation", value.ID)
		}
	}
}

// 网络结果可以晚于目录换代返回, 使用最新目录索引拒绝旧实例, 新实例仍必须明确显示为未采集.
func TestMetricReplacement(t *testing.T) {
	started := make(chan struct{})
	release := make(chan struct{}, 1)
	server := httptest.NewServer(http.HandlerFunc(func(response http.ResponseWriter, request *http.Request) {
		close(started)
		select {
		case <-release:
		case <-request.Context().Done():
			return
		}
		response.Header().Set("X-Astra-Instance", "old")
		_, _ = response.Write([]byte(complete()))
	}))
	defer server.Close()
	defer close(release)
	ctx, cancel := context.WithTimeout(t.Context(), 3*time.Second)
	defer cancel() // 错误退出时先取消 HTTP, 不让夹具等待被阻塞的处理器.
	backend := &Backend{nodes: []node{{ID: "old", Role: "ROLE_STAR", Endpoint: "endpoint"}}, members: map[string]string{"endpoint": "old"}}
	done := make(chan struct{})
	go func() {
		defer close(done)
		backend.sample(ctx, server.Client(), map[string]string{"endpoint": server.URL})
	}()
	select {
	case <-started:
	case <-ctx.Done():
		t.Fatal("scrape never started")
	}
	backend.mutex.Lock()
	backend.nodes = []node{{ID: "new", Role: "ROLE_STAR", Endpoint: "endpoint"}}
	backend.members = map[string]string{"endpoint": "new"}
	backend.mutex.Unlock()
	release <- struct{}{}
	select {
	case <-done:
	case <-ctx.Done():
		t.Fatal("replaced scrape did not stop")
	}
	if len(backend.metrics) != 0 {
		t.Fatal("late response was installed for a replaced instance")
	}
	encoded, err := json.Marshal(backend.Metrics())
	if err != nil || !strings.Contains(string(encoded), `"id":"new"`) || !strings.Contains(string(encoded), `"stale":true`) || strings.Contains(string(encoded), `"id":"old"`) {
		t.Fatal("replacement was omitted or given the old observation", err)
	}
}

// 缓存与展示使用同一采样事件, 但仅输出副本转为 UTC, 保证新鲜度仍按本地单调时间判断.
func TestMetricFreshness(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(response http.ResponseWriter, _ *http.Request) {
		response.Header().Set("X-Astra-Instance", "current")
		_, _ = response.Write([]byte(complete()))
	}))
	defer server.Close()
	backend := &Backend{nodes: []node{{ID: "current", Role: "ROLE_STAR", Endpoint: "endpoint"}}, members: map[string]string{"endpoint": "current"}}
	backend.sample(t.Context(), server.Client(), map[string]string{"endpoint": server.URL})
	observed := backend.metrics["endpoint"]
	if observed.Stale || observed.Observed.IsZero() || observed.Observed == observed.Observed.Round(0) {
		t.Fatal("successful observation lost its monotonic reading")
	}

	for _, age := range []time.Duration{0, 20 * time.Second} {
		cached := observed
		cached.Observed = cached.Observed.Add(-age)
		backend.metrics["endpoint"] = cached
		encoded, err := json.Marshal(backend.Metrics())
		if err != nil {
			t.Fatal(err)
		}
		var output struct {
			Samples []metric `json:"samples"`
		}
		if err := json.Unmarshal(encoded, &output); err != nil || len(output.Samples) != 1 {
			t.Fatal("invalid metrics projection", err)
		}
		value := output.Samples[0]
		if value.Stale != (age != 0) || value.Observed.Location() != time.UTC || !value.Observed.Equal(cached.Observed) || backend.metrics["endpoint"].Observed != cached.Observed {
			t.Fatal("freshness or UTC projection changed the cached observation")
		}
	}
}
