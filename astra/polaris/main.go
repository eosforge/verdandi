// Polaris 是唯一 Almanac 持久发布权威, 仅启动内部 TLS/gRPC 服务, 不提供公共 Comet 写接口.
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"log/slog"
	"net"
	"os"
	"os/signal"
	"runtime"
	"syscall"
	"time"

	"github.com/eosforge/verdandi/astra/internal/admission"
	"github.com/eosforge/verdandi/astra/internal/command"
	"github.com/eosforge/verdandi/astra/internal/generated/orbit"
	"github.com/eosforge/verdandi/astra/internal/generated/polaris"
	"github.com/eosforge/verdandi/astra/polaris/internal/server"
	"github.com/eosforge/verdandi/astra/polaris/internal/storage"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials"
	"google.golang.org/grpc/health"
	"google.golang.org/grpc/health/grpc_health_v1"
	"google.golang.org/grpc/keepalive"
)

// options 保存已解析部署配置, 参数不包含明文密码, 账号材料仍来自 identity/login.json.
type options struct {
	listen, advertise, super, galaxy, group, identity, state string
	initialize                                               bool
	snapshots                                                int64
}

// parse 不读取凭据或创建数据库, help 可以在没有工具链/部署环境的机器上查看.
func parse(arguments []string) (options, error) {
	var config options
	flags := flag.NewFlagSet("polaris", flag.ContinueOnError)
	flags.StringVar(&config.listen, "listen", "", "Internal numeric IP:PORT listener")
	flags.StringVar(&config.advertise, "advertise", "", "Canonical advertised IP:PORT; defaults to listen")
	flags.StringVar(&config.super, "super", "", "Pulsar admission IP:PORT")
	flags.StringVar(&config.galaxy, "galaxy", "", "Galaxy name")
	flags.StringVar(&config.group, "group", "default", "Logical locality group")
	flags.StringVar(&config.identity, "identity", "identity", "TLS, admission.pub and login.json directory")
	flags.StringVar(&config.state, "state", "state/polaris.db", "Existing exclusive authority SQLite database")
	flags.BoolVar(&config.initialize, "init", false, "Explicitly create a new authority database; refuses an existing file")
	flags.Int64Var(&config.snapshots, "snapshot-bytes", 256<<20, "Shared preparation and active snapshot budget")
	if err := command.Parse(flags, arguments); err != nil {
		return options{}, err
	}
	if config.advertise == "" {
		config.advertise = config.listen
	}
	for _, endpoint := range []string{config.listen, config.advertise, config.super} {
		if _, err := admission.Endpoint(endpoint); err != nil {
			return options{}, errors.New("listen, advertise and super require canonical concrete IP:PORT")
		}
	}
	if flags.NArg() != 0 || !admission.Name(config.galaxy) || !admission.Name(config.group) || config.state == "" || config.identity == "" || config.snapshots < 80<<20 || config.snapshots > 1<<30 {
		return options{}, errors.New("invalid Polaris configuration")
	}
	return config, nil
}

// run 的关闭顺序为 RPC -> 真实流 worker -> 目录刷新 -> Channel -> 数据库, 所有资源属于本进程.
func run(ctx context.Context, config options) error {
	if runtime.GOOS != "linux" {
		return errors.New("Polaris currently requires Linux with the project CGO toolchain")
	}
	identity, err := admission.Load(config.identity, config.advertise)
	if err != nil {
		return err
	}
	store, err := storage.Open(ctx, config.state, storage.Binding{Galaxy: config.galaxy, Username: identity.Username(), Advertise: config.advertise}, storage.Default(), config.initialize)
	if err != nil {
		return err
	}
	defer store.Close()
	if err := server.Validate(ctx, store); err != nil {
		return err
	}
	node, err := admission.Open(identity, config.super, config.galaxy, config.advertise, config.group, orbit.Role_ROLE_POLARIS)
	if err != nil {
		return err
	}
	defer node.Close()
	// 先占用受证书授权的实际端口, 防止已登记后才发现绑定失败; gRPC 只在完整初始化后 Serve.
	listener, err := net.Listen("tcp", config.listen)
	if err != nil {
		return errors.New("cannot bind Polaris internal listener")
	}
	defer listener.Close()
	if err := node.Join(ctx); err != nil {
		return err
	}
	management, err := server.NewAuthority(store, node.Directory(), config.snapshots)
	if err != nil {
		return err
	}
	streams := server.NewStreams(node, management)
	rpc := grpc.NewServer(grpc.Creds(credentials.NewTLS(identity.Server())), grpc.MaxRecvMsgSize(8<<20), grpc.MaxSendMsgSize(8<<20), grpc.MaxConcurrentStreams(32), grpc.MaxHeaderListSize(8192), grpc.ConnectionTimeout(3*time.Second), grpc.KeepaliveParams(keepalive.ServerParameters{Time: 30 * time.Second, Timeout: 10 * time.Second}), grpc.KeepaliveEnforcementPolicy(keepalive.EnforcementPolicy{MinTime: 10 * time.Second, PermitWithoutStream: true}))
	polaris.RegisterAuthorityServer(rpc, management)
	polaris.RegisterAlmanacServer(rpc, streams)
	healthy := health.NewServer()
	grpc_health_v1.RegisterHealthServer(rpc, healthy)
	healthy.SetServingStatus("", grpc_health_v1.HealthCheckResponse_SERVING)
	ctx, cancel := context.WithCancel(ctx)
	defer cancel()
	refresh := make(chan error, 1)
	go func() {
		refresh <- node.Run(ctx, func(err error) {
			if err != nil {
				slog.Warn("Pulsar directory refresh unavailable; retaining observed membership")
			}
		})
	}()
	serving := make(chan error, 1)
	go func() { serving <- rpc.Serve(listener) }()
	slog.Info("Polaris ready", "endpoint", config.advertise, "galaxy", config.galaxy, "instance", node.Member().Id)
	var result error
	refreshed := false
	select {
	case <-ctx.Done():
	case result = <-refresh:
		refreshed = true
	case result = <-serving:
	}
	// 失去自身准入或退出时先停止新 RPC. 5 秒平滑退出预算后强制取消剩余慢流.
	healthy.Shutdown()
	cancel()
	stopped := make(chan struct{})
	go func() {
		rpc.GracefulStop()
		close(stopped)
	}()
	select {
	case <-stopped:
	case <-time.After(5 * time.Second):
		rpc.Stop()
		<-stopped
	}
	streams.Wait()
	if !refreshed {
		<-refresh
	}
	if errors.Is(result, context.Canceled) || errors.Is(result, grpc.ErrServerStopped) {
		return nil
	}
	return result
}

// main 只向终端报告固定分类和必要部署上下文, 不打印密码、APISECRET 或 SQL 绑定参数.
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
		slog.Error("Polaris stopped", "error", err)
		os.Exit(1)
	}
}
