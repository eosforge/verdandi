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
	listen, advertise, super, galaxy, group, identity, state string // 监听/播报/准入地址、集群/分组、身份目录、库路径.
	initialize                                               bool   // 是否显式初始化新库 (拒绝已存在文件).
	snapshots                                                int64  // 快照共享准备预算 (字节).
}

// parse 不读取凭据或创建数据库, help 可以在没有工具链/部署环境的机器上查看.
// arguments 为命令行参数 (不含程序名); 返回校验后的部署配置, 非法时返回错误.
func parse(arguments []string) (options, error) {
	var config options
	flags := flag.NewFlagSet("polaris", flag.ContinueOnError)
	// 注册全部命令行选项, 默认值即安全基线, 帮助文本面向部署者.
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
	// 未显式播报地址则默认与监听一致.
	if config.advertise == "" {
		config.advertise = config.listen
	}
	// 三个网络端点必须为规范 IP:PORT; 快照预算有界 (80 MiB..1 GiB); 无多余参数; 命名与路径合法.
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
// ctx 为进程级上下文; config 为已校验的部署配置; 返回运行结果, 正常关闭返回空.
func run(ctx context.Context, config options) error {
	// 运行环境限定 Linux + CGO 工具链 (flock 独占依赖).
	if runtime.GOOS != "linux" {
		return errors.New("Polaris currently requires Linux with the project CGO toolchain")
	}
	// 按序准备: 身份材料、权威库、内部登录底稿、准入节点, 任一步失败直接返回.
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
	// 加入准入目录, 之后构造管理服务与推送流服务.
	if err := node.Join(ctx); err != nil {
		return err
	}
	management, err := server.NewAuthority(store, node.Directory(), config.snapshots)
	if err != nil {
		return err
	}
	streams := server.NewStreams(node, management)
	// gRPC 服务参数固定: 内部 TLS、8 MiB 消息、32 并发流、保活 30/10 秒.
	rpc := grpc.NewServer(grpc.Creds(credentials.NewTLS(identity.Server())), grpc.MaxRecvMsgSize(8<<20), grpc.MaxSendMsgSize(8<<20), grpc.MaxConcurrentStreams(32), grpc.MaxHeaderListSize(8192), grpc.ConnectionTimeout(3*time.Second), grpc.KeepaliveParams(keepalive.ServerParameters{Time: 30 * time.Second, Timeout: 10 * time.Second}), grpc.KeepaliveEnforcementPolicy(keepalive.EnforcementPolicy{MinTime: 10 * time.Second, PermitWithoutStream: true}))
	polaris.RegisterAuthorityServer(rpc, management)
	polaris.RegisterAlmanacServer(rpc, streams)
	// 健康检查常设 SERVING, 关闭时先置非服务再排空.
	healthy := health.NewServer()
	grpc_health_v1.RegisterHealthServer(rpc, healthy)
	healthy.SetServingStatus("", grpc_health_v1.HealthCheckResponse_SERVING)
	// 目录刷新与服务后台并行, 错误写入缓冲通道供主循环.
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
	// 主循环等待三者之一: 父取消、目录任务结束、服务结束.
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
	// 等待推送流真实资源退出, 再等目录任务, 最后判定正常关闭.
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
	// 日志固定 JSON 输出到标准输出.
	slog.SetDefault(slog.New(slog.NewJSONHandler(os.Stdout, nil)))
	config, err := parse(os.Args[1:])
	if errors.Is(err, flag.ErrHelp) {
		// 帮助请求正常退出.
		return
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
	// 中断与终止信号转为上下文取消, 驱动 run 的有序关闭.
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	if err := run(ctx, config); err != nil && !errors.Is(err, context.Canceled) {
		slog.Error("Polaris stopped", "error", err)
		os.Exit(1)
	}
}
