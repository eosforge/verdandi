package admission

import (
	"context"
	"errors"
	"log/slog"
	"net"
	"time"

	wire "github.com/eosforge/verdandi/supervisor/internal/generated"
	"github.com/eosforge/verdandi/supervisor/internal/membership"
	"golang.org/x/net/netutil"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials"
	"google.golang.org/grpc/keepalive"
	"google.golang.org/grpc/status"
)

// 请求和响应使用不同预算, 名单对象另受 Store 的成员数限制.
const MaxRequestBytes = 4096
const MaxResponseBytes = 2 * 1024 * 1024

// Server 借用只读认证配置和持久成员表, 由 app 在 Serve 返回后释放存储.
type Server struct {
	wire.UnimplementedAdmissionServer
	// Galaxy 是启动验证的唯一 Galaxy, 不接受请求切换群组.
	Galaxy string
	// Authority 借用启动后不可变的 TLS, 账号和签名配置, 必须由 Load 取得.
	Authority *Authority
	// Store 借用 app 独占的数据库, Serve 结束后才能关闭.
	Store *membership.Store
	// MaximumConnections 为 1..65536, 包含尚未完成 TLS 的连接, 零非法.
	MaximumConnections int
	// Logger 借用并发安全的结构化日志目标, 不记录请求或账号材料.
	Logger *slog.Logger
}

// Serve 拥有监听器和 gRPC 后台连接. 停机等待 handler 返回后, app 才能关闭数据库.
func (s *Server) Serve(ctx context.Context, listener net.Listener) error {
	return s.serve(ctx, listener, 5*time.Second)
}

// serve 共用生产处理链; 白盒 race 夹具只放宽 RPC 预算, 不替换密码验证或复制 gRPC 服务设置.
// requestTimeout 必须为正; 生产入口固定五秒, 不暴露 CLI 或每节点可变的认证计时状态.
func (s *Server) serve(ctx context.Context, listener net.Listener, requestTimeout time.Duration) error {
	if ctx == nil || listener == nil || !membership.Name(s.Galaxy) || s.Authority == nil || s.Authority.TLS == nil ||
		s.Authority.accounts == nil || s.Store == nil || s.Logger == nil || s.MaximumConnections < 1 || s.MaximumConnections > 65536 || requestTimeout <= 0 {
		if listener != nil {
			_ = listener.Close()
		}
		return errors.New("invalid admission server configuration")
	}
	server := grpc.NewServer(
		grpc.Creds(credentials.NewTLS(s.Authority.TLS)), grpc.ConnectionTimeout(5*time.Second),
		grpc.MaxRecvMsgSize(MaxRequestBytes), grpc.MaxSendMsgSize(MaxResponseBytes), grpc.MaxConcurrentStreams(1),
		grpc.MaxHeaderListSize(4096), grpc.WaitForHandlers(true),
		grpc.KeepaliveParams(keepalive.ServerParameters{MaxConnectionIdle: 5 * time.Second, MaxConnectionAge: 15 * time.Second, MaxConnectionAgeGrace: time.Second}),
		grpc.UnaryInterceptor(func(ctx context.Context, request any, _ *grpc.UnaryServerInfo, handler grpc.UnaryHandler) (any, error) {
			limited, cancel := context.WithTimeout(ctx, requestTimeout)
			defer cancel()
			return handler(limited, request)
		}),
	)
	wire.RegisterAdmissionServer(server, s)
	owned := &ownedListener{Listener: netutil.LimitListener(listener, s.MaximumConnections), connections: make(map[*ownedConnection]struct{})}
	stopped := make(chan struct{})
	stop := context.AfterFunc(ctx, func() { _ = owned.Close(); server.Stop(); close(stopped) })
	defer func() {
		if stop() {
			_ = owned.Close()
			server.Stop()
		} else {
			<-stopped
		}
	}()
	s.Logger.Info("supervisor_registration_started", "listen", listener.Addr().String(), "galaxy", s.Galaxy)
	err := server.Serve(owned)
	if ctx.Err() != nil || errors.Is(err, grpc.ErrServerStopped) {
		return nil
	}
	return err
}

// Register 一次认证后原子登记, 返回已提交身份和完整快照. 幂等键不进入凭证或日志.
func (s *Server) Register(ctx context.Context, request *wire.RegistrationRequest) (*wire.RegistrationResponse, error) {
	if request.Role != wire.Role_ROLE_STAR && request.Role != wire.Role_ROLE_PLANET {
		return nil, status.Error(codes.InvalidArgument, "explicit role required")
	}
	if err := s.Authority.accounts.authenticate(ctx, request.Username, request.Password, request.Role); err != nil {
		return nil, err
	}
	if request.Galaxy != s.Galaxy || !membership.Address(request.Advertise) || !membership.Name(request.Group) {
		return nil, status.Error(codes.InvalidArgument, "invalid registration target")
	}
	role := membership.Star
	if request.Role == wire.Role_ROLE_PLANET {
		role = membership.Planet
	}
	principal := endpointPrincipal(request.Username, s.Galaxy, request.Advertise)
	if len(request.RequestId) != 32 {
		return nil, status.Error(codes.InvalidArgument, "invalid startup request")
	}
	id, err := newID()
	if err != nil {
		return nil, status.Error(codes.Internal, "identity issuance failed")
	}
	members, err := s.Store.Register(s.Galaxy,
		membership.Member{ID: id, Principal: principal, Address: request.Advertise, Role: role, Group: request.Group}, request.RequestId)
	if err != nil {
		return nil, storeError(err)
	}
	result := &wire.RegistrationResponse{}
	for _, member := range members {
		encoded := encodeMember(s.Galaxy, member)
		if member.Role == membership.Star {
			result.Members = append(result.Members, encoded)
		}
		if member.Principal == principal {
			result.Admission, result.Signature, err = s.Authority.Sign(encoded)
			if err != nil {
				return nil, status.Error(codes.Internal, "credential unavailable")
			}
		}
	}
	if role == membership.Planet {
		result.Members = candidates(result.Members, request.Group, principal, request.CandidateRound)
	}
	return result, nil
}

// 线上仅提供有限分类, 不泄露数据库路径或请求中的账号材料.
func storeError(err error) error {
	code := codes.Unavailable
	switch {
	case errors.Is(err, membership.ErrConflict):
		code = codes.Aborted
	case errors.Is(err, membership.ErrCapacity):
		code = codes.ResourceExhausted
	case errors.Is(err, membership.ErrInvalid):
		code = codes.InvalidArgument
	}
	return status.Error(code, "registration rejected")
}
