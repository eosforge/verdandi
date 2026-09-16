package app

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"time"

	"golang.org/x/net/netutil"

	"github.com/eosforge/verdandi/supervisor/internal/admission"
	"github.com/eosforge/verdandi/supervisor/internal/management"
	"github.com/eosforge/verdandi/supervisor/internal/membership"
)

// Run 校验配置,绑定管理端口, 并阻塞到 ctx 取消或 HTTP 服务异常退出.
// ctx 与 logger 必须非 nil. 正常取消完成关闭后返回 nil, 启动或关闭失败返回错误.
// 函数拥有 listener 和 Serve goroutine, 返回前关闭连接并收取 Serve 结果.
func Run(ctx context.Context, cfg Config, logger *slog.Logger) (result error) {
	if ctx == nil || logger == nil {
		return errors.New("context and logger are required")
	}
	if err := cfg.Validate(); err != nil {
		return err
	}
	// 已取消的启动不打开端口, 与运行期间收到关闭请求采用相同的正常退出语义.
	if ctx.Err() != nil {
		return nil
	}
	listener, err := (&net.ListenConfig{}).Listen(ctx, "tcp", cfg.Listen)
	if err != nil {
		return fmt.Errorf("listen: %w", err)
	}
	if cfg.StarListen == "" {
		return serve(ctx, cfg, logger, listener, management.Handler())
	}
	// 登记相关资源按所有权反序释放. 任一步失败都关闭已绑定管理端口.
	defer listener.Close()
	authority, err := admission.Load(cfg.Identity)
	if err != nil {
		return err
	}
	store, err := membership.Open(cfg.Members, cfg.MaxMembers, cfg.MaxStartups)
	if err != nil {
		return err
	}
	defer func() { result = errors.Join(result, store.Close()) }()
	starListener, err := (&net.ListenConfig{}).Listen(ctx, "tcp", cfg.StarListen)
	if err != nil {
		return err
	}
	defer starListener.Close()
	lifetime, cancel := context.WithCancel(ctx)
	defer cancel()
	registration := &admission.Server{Galaxy: cfg.Galaxy, Authority: authority, Store: store, MaximumConnections: cfg.MaxConnections, Logger: logger}
	results := make(chan error, 2)
	go func() { results <- registration.Serve(lifetime, starListener) }()
	go func() { results <- serve(lifetime, cfg, logger, listener, management.Handler()) }()
	// 任一服务退出均停止另一个, 并等待两者归还所有任务, 然后才关闭成员数据库.
	first := <-results
	cancel()
	return errors.Join(first, <-results)
}

// serve 接管已绑定 listener, 使用独立请求上下文让正常关闭先等待在途请求.
// handler 必须遵循请求上下文取消, 不得持有脱离服务生命周期的后台任务.
func serve(ctx context.Context, cfg Config, logger *slog.Logger, listener net.Listener, handler http.Handler) error {
	// 配置已保证名额为正. netutil 在 Accept 前等待名额, 连接关闭时归还, listener 关闭时唤醒等待者.
	listener = netutil.LimitListener(listener, cfg.MaxConnections)
	defer listener.Close()
	requestCtx, cancelRequests := context.WithCancel(context.Background())
	defer cancelRequests()
	server := &http.Server{
		Handler:           handler,
		ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout:       10 * time.Second,
		WriteTimeout:      10 * time.Second,
		IdleTimeout:       30 * time.Second,
		MaxHeaderBytes:    16 * 1024,
		BaseContext:       func(net.Listener) context.Context { return requestCtx },
		ErrorLog:          slog.NewLogLogger(logger.Handler(), slog.LevelWarn),
	}

	// 单个容量为一的结果通道由 Serve 写入, 主生命周期始终收取结果.
	served := make(chan error, 1)
	go func() { served <- server.Serve(listener) }()
	logger.Info("supervisor_started", "listen", listener.Addr().String(), "scope", "management")
	var serveErr error
	var finished bool
	select {
	case serveErr = <-served:
		finished = true
	case <-ctx.Done():
	}

	// 使用新的有界上下文排空请求, 不能复用已取消的进程上下文.
	logger.Info("supervisor_stopping")
	shutdownCtx, cancelShutdown := context.WithTimeout(context.Background(), cfg.ShutdownTimeout)
	shutdownErr := server.Shutdown(shutdownCtx)
	cancelShutdown()
	if shutdownErr != nil {
		cancelRequests()
		shutdownErr = errors.Join(shutdownErr, server.Close())
	}
	if !finished {
		serveErr = <-served
	}
	// ErrServerClosed 是主动停止 listener 的正常结果, 其他 Serve 错误不能吞掉.
	if errors.Is(serveErr, http.ErrServerClosed) {
		serveErr = nil
	}
	err := errors.Join(serveErr, shutdownErr)
	if err == nil {
		logger.Info("supervisor_stopped")
	}
	return err
}
