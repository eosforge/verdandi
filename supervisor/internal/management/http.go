// Package management 定义管理 HTTP 路由, 不拥有 listener 或后台任务.
package management

import (
	"io"
	"net/http"
)

// Handler 创建独立路由表. 探针只报告管理入口可响应, 不代表群组已登记或数据已恢复.
// 返回的 handler 不持有可变业务状态, 可以并发处理请求.
func Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /healthz", health)
	return mux
}

// health 同时处理 GET 和 HEAD, 拒绝正文避免探针成为无界上传入口.
func health(w http.ResponseWriter, r *http.Request) {
	if r.ContentLength != 0 {
		w.Header().Set("Connection", "close")
		http.Error(w, "health probe does not accept a body", http.StatusBadRequest)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	// 固定有界正文, 断线产生的写错误由 HTTP 生命周期收尾.
	_, _ = io.WriteString(w, "{\"status\":\"ok\",\"component\":\"supervisor\",\"scope\":\"management\"}\n")
}
