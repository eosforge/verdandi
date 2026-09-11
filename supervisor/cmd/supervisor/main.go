// supervisor 是独立 Go 管理服务的命令行入口.
// 本层只拥有参数,日志和进程信号, HTTP 生命周期由 internal/app 管理.
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"log/slog"
	"os"
	"os/signal"
	"syscall"

	"github.com/eosforge/verdandi/supervisor/internal/admission"
	"github.com/eosforge/verdandi/supervisor/internal/app"
)

// main 监听 Ctrl+C 和 SIGTERM, 返回前释放信号订阅, 再设置进程退出码.
func main() {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	code := run(ctx, os.Args[1:], os.Stdout, os.Stderr)
	stop()
	os.Exit(code)
}

// run 将参数或配置错误映射为 2, 运行错误映射为 1, 帮助及正常关闭映射为 0.
// stdout 接收 JSON 日志, stderr 接收参数诊断; 调用方拥有这两个 writer, 本函数不关闭它们.
func run(ctx context.Context, args []string, stdout, stderr io.Writer) int {
	// 单独模式只处理 stdin, 不绑定端口或加载服务私钥.
	if len(args) == 1 && args[0] == "--make-account" {
		if err := admission.WriteAccount(os.Stdin, stdout); err != nil {
			fmt.Fprintln(stderr, "supervisor:", err)
			return 2
		}
		return 0
	}
	cfg := app.DefaultConfig()
	level := slog.LevelInfo
	flags := flag.NewFlagSet("supervisor", flag.ContinueOnError)
	flags.SetOutput(stderr)
	flags.StringVar(&cfg.Listen, "listen", cfg.Listen, "management HTTP IP:PORT or [IPv6]:PORT")
	flags.DurationVar(&cfg.ShutdownTimeout, "shutdown-timeout", cfg.ShutdownTimeout, "graceful shutdown timeout (0, 1m]")
	flags.IntVar(&cfg.MaxConnections, "max-connections", cfg.MaxConnections, "maximum accepted HTTP connections [1, 65536]")
	flags.StringVar(&cfg.PeerListen, "peer-listen", "", "registration gRPC/TLS IP:PORT; empty means management only")
	flags.StringVar(&cfg.Cluster, "cluster", "", "authorized registration cluster")
	flags.StringVar(&cfg.Identity, "identity", "", "directory containing TLS identity, signing key and accounts.json")
	flags.StringVar(&cfg.Members, "members", "", "persistent member database file")
	flags.IntVar(&cfg.MaxPeers, "max-peers", cfg.MaxPeers, "maximum registered peers in the cluster [1, 4096]")
	flags.TextVar(&level, "log-level", level, "DEBUG, INFO, WARN or ERROR")
	version := flags.Bool("version", false, "show the service version")
	// 标准 flag 支持 --name=value; 不自行维护另一套转义或缺值解析器.
	if err := flags.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return 0
		}
		return 2
	}
	if flags.NArg() != 0 {
		fmt.Fprintln(stderr, "supervisor: positional arguments are not supported")
		return 2
	}
	if *version {
		if _, err := fmt.Fprintln(stdout, "supervisor 0.1.0"); err != nil {
			return 1
		}
		return 0
	}
	if err := cfg.Validate(); err != nil {
		fmt.Fprintln(stderr, "supervisor:", err)
		return 2
	}

	// 日志只包含当前管理服务事实, 不把 HTTP 启动标记为 Peer 群组已经 Ready.
	logger := slog.New(slog.NewJSONHandler(stdout, &slog.HandlerOptions{Level: level}))
	if err := app.Run(ctx, cfg, logger); err != nil {
		logger.Error("supervisor_failed", "error", err)
		return 1
	}
	return 0
}
