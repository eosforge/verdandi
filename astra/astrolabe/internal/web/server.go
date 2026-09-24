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
	Origins   []string // 允许的浏览器源列表 (1..32 个), 启动时规范化去重.
	Secure    bool     // 是否 HTTPS, 决定 Cookie Secure 标志.
	Crosssite bool     // 是否跨站 Cookie, 仅 HTTPS 下允许.
	Sessions  int      // 会话上限 (1..4096).
}

// Server 没有数据库和后台持久队列; 普通请求只占短会话查找, KDF 和管理 RPC 分别限流.
type Server struct {
	account   *Account        // 单个管理账号摘要, 登录验证用.
	backend   Backend         // 权威后端 (真实为 bridge.Backend), 提交/目录/快照/节点/指标.
	origins   map[string]bool // 允许源集合, 由 Options 规范化而来.
	secure    bool            // 是否 HTTPS, 透传给 Cookie.
	crosssite bool            // 是否跨站 Cookie 模式.
	sessions  sessions        // 内存会话表, 八小时固定期限.
	logins    chan struct{}   // KDF 并发许可 (4), 与普通请求分离.
	requests  chan struct{}   // 普通已认证请求并发许可 (16).
	loads     chan struct{}   // 快照加载并发许可 (2), 最重操作单独限流.
}

// Origin 归一化 scheme/host/default-port, 拒绝路径、用户信息、通配、null 与伪装子域后缀.
// value 为待校验源文本; 返回规范源, 非法时返回错误.
func Origin(value string) (string, error) {
	// 先做结构校验: 协议仅 http/https, 无用户信息、路径、查询、片段.
	parsed, err := url.Parse(value)
	if err != nil || parsed.Scheme != "https" && parsed.Scheme != "http" || parsed.Hostname() == "" || parsed.User != nil || parsed.Path != "" || parsed.RawQuery != "" || parsed.ForceQuery || parsed.Fragment != "" || parsed.Opaque != "" {
		return "", errInput
	}
	// 主机名小写化并拒绝通配与空白类字符, 尾冒号视作非法端口写法.
	host, port := strings.ToLower(parsed.Hostname()), parsed.Port()
	if strings.ContainsAny(host, "*%\\ \t\r\n") || strings.HasSuffix(parsed.Host, ":") {
		return "", errInput
	}
	if port != "" {
		// 端口必须为规范十进制非零 16 位, 变体写法拒绝.
		number, err := strconv.ParseUint(port, 10, 16)
		if err != nil || number == 0 || strconv.FormatUint(number, 10) != port {
			return "", errInput
		}
	}
	// 默认端口省略, IPv6 无端口时补方括号, 输出统一形式.
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
// account/backend 为依赖; options 为策略配置; 返回可服务的 Server, 非法时返回错误.
func New(account *Account, backend Backend, options Options) (*Server, error) {
	// 容量与策略先行校验: 账号后端非空、会话数有界、源数量有界、跨站必须 HTTPS.
	if account == nil || backend == nil || options.Sessions < 1 || options.Sessions > 4096 || len(options.Origins) == 0 || len(options.Origins) > 32 || options.Crosssite && !options.Secure {
		return nil, errInput
	}
	// 源逐个规范化去重, 重复或非法直接拒绝.
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
// response/request 为当前 HTTP 交换; 按来源、方法、会话、配额逐层放行后分发路由.
func (server *Server) ServeHTTP(response http.ResponseWriter, request *http.Request) {
	// 安全响应头对所有响应生效, 包括错误路径.
	response.Header().Set("Cache-Control", "no-store")
	response.Header().Set("X-Content-Type-Options", "nosniff")
	response.Header().Add("Vary", "Origin")
	// 来源检查: 无 Origin 放行 (非浏览器), 单一 Origin 必须在允许集合中才回 CORS 头.
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
	// 预检请求单独处理, 不进入认证与路由.
	if request.Method == http.MethodOptions {
		server.preflight(response, request, allowed)
		return
	}
	// 写方法必须通过来源与请求头双重确认 (防 CSRF), 读方法只要求其一.
	if request.Method != http.MethodGet && request.Method != http.MethodHead && (!allowed || len(request.Header.Values("X-Astra-Request")) != 1 || request.Header.Get("X-Astra-Request") != "1") {
		problem(response, http.StatusForbidden, "origin", "unapplied")
		return
	}
	// 会话端点无需已登录态, 其他路径必须持有有效会话.
	if request.URL.Path == "/api/session" {
		server.session(response, request)
		return
	}
	if _, ok := server.sessions.get(token(request), time.Now()); !ok {
		problem(response, http.StatusUnauthorized, "session", "unapplied")
		return
	}
	// 普通已认证请求占配额, 满时返回忙.
	select {
	case server.requests <- struct{}{}:
		defer func() { <-server.requests }()
	default:
		problem(response, http.StatusTooManyRequests, "busy", "unapplied")
		return
	}
	// 按路径分发, 节点与指标只接受 GET.
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
// allowed 为来源是否已通过检查; 只放行 GET/POST/DELETE 与两个固定请求头.
func (server *Server) preflight(response http.ResponseWriter, request *http.Request, allowed bool) {
	// 请求方法必须唯一且在允许集合中, 否则拒绝预检.
	method := request.Header.Get("Access-Control-Request-Method")
	if !allowed || method != "GET" && method != "POST" && method != "DELETE" || len(request.Header.Values("Access-Control-Request-Method")) != 1 {
		problem(response, http.StatusForbidden, "preflight", "unapplied")
		return
	}
	// 请求头白名单仅 content-type 与 x-astra-request, 其他一律拒绝.
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
// response/request 为当前 HTTP 交换; POST 登录、DELETE 注销、GET 查询.
func (server *Server) session(response http.ResponseWriter, request *http.Request) {
	switch request.Method {
	case http.MethodPost:
		// 登录输入有界: 用户名 1..64, 密码 1..1024, 超限直接拒绝.
		var input struct {
			Username string `json:"username"`
			Password string `json:"password"`
		}
		if body(response, request, 4096, &input) != nil || len(input.Username) == 0 || len(input.Username) > 64 || len(input.Password) == 0 || len(input.Password) > 1024 {
			problem(response, http.StatusBadRequest, "input", "unapplied")
			return
		}
		// KDF 占独立许可, 满时返回忙, 不挤占普通请求配额.
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
			// 客户端已断开, 不再创建无用的会话.
			return
		}
		// 验证通过后创建会话并替换本浏览器旧令牌, 满容量时创建失败返回忙.
		value, expire, ok := server.sessions.create(time.Now(), token(request))
		if !ok {
			problem(response, http.StatusTooManyRequests, "busy", "unapplied")
			return
		}
		server.cookie(response, value, expire)
		respond(response, map[string]any{"username": server.account.username, "expires": expire.UTC()})
	case http.MethodDelete:
		// 注销幂等: 先撤销服务端会话再清 Cookie, 缺项也不报错.
		server.sessions.remove(token(request))
		server.cookie(response, "", time.Time{})
		response.WriteHeader(http.StatusNoContent)
	case http.MethodGet:
		// 会话查询返回用户名与到期时间, 无效会话返回未认证.
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
// maximum 为正文上限; target 为解码目标; 返回错误时调用方直接报输入错误.
func body(response http.ResponseWriter, request *http.Request, maximum int64, target any) error {
	// 内容类型必须为 JSON, 其他类型直接拒绝.
	media, _, err := mime.ParseMediaType(request.Header.Get("Content-Type"))
	if err != nil || media != "application/json" {
		return errInput
	}
	// 限长读取, 超限由 MaxBytesReader 截断并报错.
	request.Body = http.MaxBytesReader(response, request.Body, maximum)
	data, err := io.ReadAll(request.Body)
	if err != nil {
		return errInput
	}
	return object(data, target)
}

// respond 不在错误里回显用户输入、后端原文或 SQL; 写出失败不会重新执行管理操作.
// value 为待序列化响应.
func respond(response http.ResponseWriter, value any) {
	response.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(response).Encode(value)
}

// problem 的 code/effect 均由实现固定选择, unknown 表示不得把失败当成未提交.
// code 为 HTTP 状态码; reason 为原因词; effect 为效果词 (unapplied/unknown).
func problem(response http.ResponseWriter, code int, reason, effect string) {
	response.Header().Set("Content-Type", "application/json")
	response.WriteHeader(code)
	respond(response, map[string]string{"error": reason, "effect": effect})
}
