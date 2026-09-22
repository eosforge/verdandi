package web

import (
	"context"
	"encoding/json"
	"io"
	"mime"
	"net"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"time"

	"github.com/eosforge/verdandi/astra/internal/generated/comet"
	"github.com/eosforge/verdandi/astra/internal/generated/polaris"
)

// Backend 的真实实现直接调用 Polaris, 测试可单独验证 HTTP 边界但不能替代跨进程持久验收.
type Backend interface {
	Commit(context.Context, *polaris.CommitRequest) (*polaris.Position, error)
	List(context.Context) (*polaris.Inventory, error)
	Load(context.Context, *comet.Scope, func(*polaris.Snapshot) error) error
	Nodes() any
	Metrics() any
}

// Options 固定管理浏览器策略和有界容量, 不从不可信 Host/Forwarded 自动推导允许来源.
type Options struct {
	Origins   []string
	Secure    bool
	Crosssite bool
	Sessions  int
}

// Server 没有数据库和后台持久队列; 普通请求只占短会话查找, KDF 和管理 RPC 分别限流.
type Server struct {
	account   *Account
	backend   Backend
	origins   map[string]bool
	secure    bool
	crosssite bool
	sessions  sessions
	logins    chan struct{}
	requests  chan struct{}
	loads     chan struct{}
}

// Origin 归一化 scheme/host/default-port, 拒绝路径、用户信息、通配、null 与伪装子域后缀.
func Origin(value string) (string, error) {
	parsed, err := url.Parse(value)
	if err != nil || parsed.Scheme != "https" && parsed.Scheme != "http" || parsed.Hostname() == "" || parsed.User != nil || parsed.Path != "" || parsed.RawQuery != "" || parsed.ForceQuery || parsed.Fragment != "" || parsed.Opaque != "" {
		return "", errInput
	}
	host, port := strings.ToLower(parsed.Hostname()), parsed.Port()
	if strings.ContainsAny(host, "*%\\ \t\r\n") || strings.HasSuffix(parsed.Host, ":") {
		return "", errInput
	}
	if port != "" {
		number, err := strconv.ParseUint(port, 10, 16)
		if err != nil || number == 0 || strconv.FormatUint(number, 10) != port {
			return "", errInput
		}
	}
	if parsed.Scheme == "https" && port == "443" || parsed.Scheme == "http" && port == "80" {
		port = ""
	}
	if port != "" {
		host = net.JoinHostPort(host, port)
	} else if strings.Contains(host, ":") {
		host = "[" + host + "]"
	}
	return parsed.Scheme + "://" + host, nil
}

// New 的容量为 1..4096 会话, 最多 32 个明确 Origin; HTTPS/跨站策略在启动时确定.
func New(account *Account, backend Backend, options Options) (*Server, error) {
	if account == nil || backend == nil || options.Sessions < 1 || options.Sessions > 4096 || len(options.Origins) == 0 || len(options.Origins) > 32 || options.Crosssite && !options.Secure {
		return nil, errInput
	}
	origins := make(map[string]bool, len(options.Origins))
	for _, value := range options.Origins {
		origin, err := Origin(value)
		if err != nil || origins[origin] {
			return nil, errInput
		}
		origins[origin] = true
	}
	return &Server{account: account, backend: backend, origins: origins, secure: options.Secure, crosssite: options.Crosssite, sessions: sessions{values: make(map[string]time.Time), maximum: options.Sessions}, logins: make(chan struct{}, 4), requests: make(chan struct{}, 16), loads: make(chan struct{}, 2)}, nil
}

// ServeHTTP 全部写入口先执行来源/请求头检查, CORS 不能只在成功响应时设置或代替实际认证.
func (server *Server) ServeHTTP(response http.ResponseWriter, request *http.Request) {
	response.Header().Set("Cache-Control", "no-store")
	response.Header().Set("X-Content-Type-Options", "nosniff")
	response.Header().Add("Vary", "Origin")
	origins := request.Header.Values("Origin")
	allowed := false
	if len(origins) == 1 {
		origin, err := Origin(origins[0])
		allowed = err == nil && server.origins[origin]
		if allowed {
			response.Header().Set("Access-Control-Allow-Origin", origins[0])
			response.Header().Set("Access-Control-Allow-Credentials", "true")
		}
	}
	if len(origins) != 0 && !allowed {
		problem(response, http.StatusForbidden, "origin", "unapplied")
		return
	}
	if request.Method == http.MethodOptions {
		server.preflight(response, request, allowed)
		return
	}
	if request.Method != http.MethodGet && request.Method != http.MethodHead && (!allowed || len(request.Header.Values("X-Astra-Request")) != 1 || request.Header.Get("X-Astra-Request") != "1") {
		problem(response, http.StatusForbidden, "origin", "unapplied")
		return
	}
	if request.URL.Path == "/api/session" {
		server.session(response, request)
		return
	}
	if _, ok := server.sessions.get(token(request), time.Now()); !ok {
		problem(response, http.StatusUnauthorized, "session", "unapplied")
		return
	}
	select {
	case server.requests <- struct{}{}:
		defer func() { <-server.requests }()
	default:
		problem(response, http.StatusTooManyRequests, "busy", "unapplied")
		return
	}
	switch request.URL.Path {
	case "/api/almanac":
		server.almanac(response, request)
	case "/api/credentials":
		server.credentials(response, request)
	case "/api/nodes", "/api/metrics":
		if request.Method != http.MethodGet {
			problem(response, http.StatusMethodNotAllowed, "method", "unapplied")
			return
		}
		if request.URL.Path == "/api/metrics" {
			respond(response, server.backend.Metrics())
		} else {
			respond(response, server.backend.Nodes())
		}
	default:
		problem(response, http.StatusNotFound, "route", "unapplied")
	}
}

// preflight 只检查来源和固定方法/头, 不要求 Cookie、不派生密码、不调用 Polaris.
func (server *Server) preflight(response http.ResponseWriter, request *http.Request, allowed bool) {
	method := request.Header.Get("Access-Control-Request-Method")
	if !allowed || method != "GET" && method != "POST" && method != "DELETE" || len(request.Header.Values("Access-Control-Request-Method")) != 1 {
		problem(response, http.StatusForbidden, "preflight", "unapplied")
		return
	}
	for _, header := range strings.Split(request.Header.Get("Access-Control-Request-Headers"), ",") {
		header = strings.ToLower(strings.TrimSpace(header))
		if header != "" && header != "content-type" && header != "x-astra-request" {
			problem(response, http.StatusForbidden, "preflight", "unapplied")
			return
		}
	}
	response.Header().Set("Access-Control-Allow-Methods", "GET, POST, DELETE")
	response.Header().Set("Access-Control-Allow-Headers", "Content-Type, X-Astra-Request")
	response.Header().Add("Vary", "Access-Control-Request-Method")
	response.Header().Add("Vary", "Access-Control-Request-Headers")
	response.WriteHeader(http.StatusNoContent)
}

// session 登录只持有 KDF 许可, 与普通会话查询及业务 RPC 配额分离.
func (server *Server) session(response http.ResponseWriter, request *http.Request) {
	switch request.Method {
	case http.MethodPost:
		var input struct {
			Username string `json:"username"`
			Password string `json:"password"`
		}
		if body(response, request, 4096, &input) != nil || len(input.Username) == 0 || len(input.Username) > 64 || len(input.Password) == 0 || len(input.Password) > 1024 {
			problem(response, http.StatusBadRequest, "input", "unapplied")
			return
		}
		select {
		case server.logins <- struct{}{}:
			defer func() { <-server.logins }()
		default:
			problem(response, http.StatusTooManyRequests, "busy", "unapplied")
			return
		}
		if !server.account.verify(input.Username, input.Password) {
			problem(response, http.StatusUnauthorized, "login", "unapplied")
			return
		}
		if request.Context().Err() != nil {
			return
		}
		value, expire, ok := server.sessions.create(time.Now(), token(request))
		if !ok {
			problem(response, http.StatusTooManyRequests, "busy", "unapplied")
			return
		}
		server.cookie(response, value, expire)
		respond(response, map[string]any{"username": server.account.username, "expires": expire.UTC()})
	case http.MethodDelete:
		server.sessions.remove(token(request))
		server.cookie(response, "", time.Time{})
		response.WriteHeader(http.StatusNoContent)
	case http.MethodGet:
		expire, ok := server.sessions.get(token(request), time.Now())
		if !ok {
			problem(response, http.StatusUnauthorized, "session", "unapplied")
			return
		}
		respond(response, map[string]any{"username": server.account.username, "expires": expire.UTC()})
	default:
		problem(response, http.StatusMethodNotAllowed, "method", "unapplied")
	}
}

// body 的 encoded 上限包括 Base64 和 JSON, 不允许未知嵌套、重复字段、多个文档或任意内容类型.
func body(response http.ResponseWriter, request *http.Request, maximum int64, target any) error {
	media, _, err := mime.ParseMediaType(request.Header.Get("Content-Type"))
	if err != nil || media != "application/json" {
		return errInput
	}
	request.Body = http.MaxBytesReader(response, request.Body, maximum)
	data, err := io.ReadAll(request.Body)
	if err != nil {
		return errInput
	}
	return object(data, target)
}

// respond 不在错误里回显用户输入、后端原文或 SQL; 写出失败不会重新执行管理操作.
func respond(response http.ResponseWriter, value any) {
	response.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(response).Encode(value)
}

// problem 的 code/effect 均由实现固定选择, unknown 表示不得把失败当成未提交.
func problem(response http.ResponseWriter, code int, reason, effect string) {
	response.Header().Set("Content-Type", "application/json")
	response.WriteHeader(code)
	respond(response, map[string]string{"error": reason, "effect": effect})
}
