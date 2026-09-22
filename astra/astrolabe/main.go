// Astrolabe 提供管理登录与 Almanac 权威提交代理, 不建立数据库或直接修改 Star 的持久权威数据.
package main

import (
	"context"
	"crypto/tls"
	"errors"
	"flag"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"github.com/eosforge/verdandi/astra/astrolabe/internal/bridge"
	"github.com/eosforge/verdandi/astra/astrolabe/internal/web"
	"github.com/eosforge/verdandi/astra/internal/admission"
	"github.com/eosforge/verdandi/astra/internal/command"
	"github.com/eosforge/verdandi/astra/internal/generated/orbit"
)

// options 的 public/origins 属于部署可信配置, 不从任意反向代理请求头推导; 密码只在 account 文件中保存摘要.
type options struct {
	listen, advertise, super, galaxy, group, identity, account, public, origins, metrics string
	plain, crosssite                                                                     bool
	sessions                                                                             int
}

// parse 只解析和验证无秘密的部署参数, --http 仅允许回环反向代理接入, 内部 Pulsar/Polaris 始终 TLS.
func parse(arguments []string) (options, error) {
	var config options
	flags := flag.NewFlagSet("astrolabe", flag.ContinueOnError)
	flags.StringVar(&config.listen, "listen", "", "Numeric management IP:PORT listener")
	flags.StringVar(&config.advertise, "advertise", "", "Backend IP:PORT registered at Pulsar; defaults to listen")
	flags.StringVar(&config.super, "super", "", "Pulsar admission IP:PORT")
	flags.StringVar(&config.galaxy, "galaxy", "", "Galaxy name")
	flags.StringVar(&config.group, "group", "default", "Logical locality group")
	flags.StringVar(&config.identity, "identity", "identity", "Internal TLS, admission.pub and login.json directory")
	flags.StringVar(&config.account, "account", "", "Existing username/salt/hash JSON for one management account")
	flags.StringVar(&config.public, "public", "", "Exact externally visible API origin, such as https://admin.example")
	flags.StringVar(&config.origins, "origins", "", "Additional comma-separated allowed browser origins")
	flags.StringVar(&config.metrics, "metrics", "", "Optional JSON map of Star endpoints to protected metrics URLs")
	flags.BoolVar(&config.plain, "http", false, "HTTP on loopback only, for a separately configured reverse proxy")
	flags.BoolVar(&config.crosssite, "crosssite", false, "Explicit SameSite=None; requires HTTPS public origin")
	flags.IntVar(&config.sessions, "sessions", 4096, "Maximum in-memory eight-hour management sessions")
	if err := command.Parse(flags, arguments); err != nil {
		return options{}, err
	}
	if config.advertise == "" {
		config.advertise = config.listen
	}
	for _, endpoint := range []string{config.listen, config.advertise, config.super} {
		if _, err := admission.Endpoint(endpoint); err != nil {
			return options{}, errors.New("listen, advertise and super require concrete canonical IP:PORT")
		}
	}
	listen, _ := admission.Endpoint(config.listen)
	public, err := web.Origin(config.public)
	if err != nil || flags.NArg() != 0 || !admission.Name(config.galaxy) || !admission.Name(config.group) || config.identity == "" || config.account == "" || config.sessions < 1 || config.sessions > 4096 || config.plain && !listen.Addr().IsLoopback() || config.crosssite && !strings.HasPrefix(public, "https://") || !config.plain && !strings.HasPrefix(public, "https://") {
		return options{}, errors.New("invalid Astrolabe configuration")
	}
	config.public = public
	origins := map[string]bool{public: true}
	if config.origins != "" {
		for _, raw := range strings.Split(config.origins, ",") {
			origin, err := web.Origin(raw)
			if err != nil || origins[origin] || len(origins) >= 32 {
				return options{}, errors.New("invalid or duplicate browser origin")
			}
			origins[origin] = true
		}
	}
	return config, nil
}

// run 先开放本地管理登录再异步准入, Pulsar 临时不可达不伪装成账号密码错误.
func run(parent context.Context, config options) error {
	targets, err := bridge.Targets(config.metrics)
	if err != nil {
		return err
	}
	identity, err := admission.Load(config.identity, config.advertise)
	if err != nil {
		return err
	}
	account, err := web.LoadAccount(config.account)
	if err != nil {
		return err
	}
	node, err := admission.Open(identity, config.super, config.galaxy, config.advertise, config.group, orbit.Role_ROLE_ASTROLABE)
	if err != nil {
		return err
	}
	defer node.Close()
	backend := bridge.New(node, identity)
	defer backend.Close()
	origins := []string{config.public}
	if config.origins != "" {
		origins = append(origins, strings.Split(config.origins, ",")...)
	}
	handler, err := web.New(account, backend, web.Options{Origins: origins, Secure: strings.HasPrefix(config.public, "https://"), Crosssite: config.crosssite, Sessions: config.sessions})
	if err != nil {
		return err
	}
	listener, err := net.Listen("tcp", config.listen)
	if err != nil {
		return errors.New("cannot bind Astrolabe management listener")
	}
	defer listener.Close()
	if !config.plain {
		listener = tls.NewListener(listener, identity.Server())
	}
	ctx, cancel := context.WithCancel(parent)
	defer cancel()
	monitored := make(chan struct{})
	go func() { defer close(monitored); backend.Monitor(ctx, targets, identity.Client()) }()
	defer func() { cancel(); <-monitored }()
	server := &http.Server{Handler: handler, ReadHeaderTimeout: 3 * time.Second, ReadTimeout: 10 * time.Second, WriteTimeout: 35 * time.Second, IdleTimeout: 60 * time.Second, MaxHeaderBytes: 8192, BaseContext: func(net.Listener) context.Context { return ctx }}
	refresh := make(chan error, 1)
	go func() {
		if err := node.Join(ctx); err != nil {
			refresh <- err
			return
		}
		backend.Update(nil)
		refresh <- node.Run(ctx, backend.Update)
	}()
	serving := make(chan error, 1)
	go func() { serving <- server.Serve(listener) }()
	slog.Info("Astrolabe management listener ready", "endpoint", config.advertise, "galaxy", config.galaxy)
	var result error
	refreshed := false
	select {
	case <-parent.Done():
	case result = <-refresh:
		refreshed = true
	case result = <-serving:
	}
	// 先取消所有所属 HTTP -> RPC 链, 再按明确期限排空服务及目录任务; 不留下请求级 goroutine.
	cancel()
	deadline, stop := context.WithTimeout(context.Background(), 5*time.Second)
	defer stop()
	if server.Shutdown(deadline) != nil {
		_ = server.Close()
	}
	if !refreshed {
		<-refresh
	}
	if errors.Is(result, context.Canceled) || errors.Is(result, http.ErrServerClosed) {
		return nil
	}
	return result
}

// main 不输出密码、Cookie、SQL 或后端原始状态正文, 信号仅关闭本进程拥有的资源.
func main() {
	slog.SetDefault(slog.New(slog.NewJSONHandler(os.Stdout, nil)))
	config, err := parse(os.Args[1:])
	if errors.Is(err, flag.ErrHelp) {
		return
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	if err := run(ctx, config); err != nil && !errors.Is(err, context.Canceled) {
		slog.Error("Astrolabe stopped", "error", err)
		os.Exit(1)
	}
}
