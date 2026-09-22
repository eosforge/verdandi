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
	backend := &Backend{nodes: []node{{ID: "current", Endpoint: "endpoint"}}, metrics: map[string]metric{"endpoint": {ID: "old", Observed: time.Now()}}}
	if value := fmt.Sprint(backend.Metrics()); strings.Contains(value, "old") {
		t.Fatal("old identity observation leaked into replacement")
	}
}

// 缓存与展示使用同一采样事件, 但仅输出副本转为 UTC, 保证新鲜度仍按本地单调时间判断.
func TestMetricFreshness(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(response http.ResponseWriter, _ *http.Request) {
		response.Header().Set("X-Astra-Instance", "current")
		_, _ = response.Write([]byte(complete()))
	}))
	defer server.Close()
	backend := &Backend{nodes: []node{{ID: "current", Role: "ROLE_STAR", Endpoint: "endpoint"}}}
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
