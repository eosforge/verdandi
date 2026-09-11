// Package app 实现 Supervisor 管理入口的配置与进程生命周期.
// 可选登记端口接入持久成员表; 管理健康检查只报告管理服务, 不伪造群组就绪.
package app

import (
	"fmt"
	"net/netip"
	"time"

	"github.com/eosforge/verdandi/supervisor/internal/membership"
)

// Config 是管理 HTTP 服务的启动配置, Run 会在打开端口前验证全部字段.
// 它按值传入, 启动后调用方的修改不影响运行中的服务.
type Config struct {
	// Listen 默认为 127.0.0.1:8080, 必须是 IP:PORT 或 [IPv6]:PORT.
	// 空字符串非法, 端口范围 0..65535, 零表示由系统分配测试端口.
	Listen string
	// ShutdownTimeout 默认为 5s, 范围 (0, 1m], 限制等待在途 HTTP 请求完成的时间.
	// 超时后取消请求并关闭连接, 零不表示无限等待.
	ShutdownTimeout time.Duration
	// MaxConnections 默认为 128, 范围 1..65536, 包含空闲和处理请求中的连接.
	// 超过上限时暂停 accept, 零不表示无限制.
	MaxConnections int
	// PeerListen 默认空, 仅管理模式; 启用登记时设置 IP:PORT, 测试允许端口零.
	PeerListen string
	// Cluster 启用登记时必填, 使用共享的群组名称约束.
	Cluster string
	// Identity 是包含部署证书和准入签名密钥的项目配置目录, 启用登记时必填.
	Identity string
	// Members 是成员数据库文件路径, 启用登记时必填, 不隐式选择全局目录.
	Members string
	// MaxPeers 默认 64, 范围 1..4096, 包含群组所有已登记部署身份.
	MaxPeers int
}

// DefaultConfig 返回可直接运行的回环地址配置, 不读取文件或修改环境.
func DefaultConfig() Config {
	return Config{Listen: "127.0.0.1:8080", ShutdownTimeout: 5 * time.Second, MaxConnections: 128, MaxPeers: 64}
}

// Validate 检查地址与资源边界, 无效输入返回带字段名的错误, 不执行网络 I/O.
func (c Config) Validate() error {
	// 仅解析本机绑定地址, 避免启动校验触发隐式 DNS 查询.
	if _, err := netip.ParseAddrPort(c.Listen); err != nil {
		return fmt.Errorf("listen: expected IP:PORT or [IPv6]:PORT: %w", err)
	}
	// 关闭必须有确定上限, 防止配置错误让退出永远等待.
	if c.ShutdownTimeout <= 0 || c.ShutdownTimeout > time.Minute {
		return fmt.Errorf("shutdown-timeout: must be greater than 0 and at most 1m")
	}
	// 连接名额决定同时存在的 HTTP 连接和相关 goroutine 数量上限.
	if c.MaxConnections < 1 || c.MaxConnections > 65536 {
		return fmt.Errorf("max-connections: must be between 1 and 65536")
	}
	// 管理模式保持独立. 部分填写登记配置属于错误, 不能悄悄忽略安全配置.
	if c.PeerListen == "" {
		if c.Cluster != "" || c.Identity != "" || c.Members != "" {
			return fmt.Errorf("peer-listen is required for registration configuration")
		}
	} else {
		if _, err := netip.ParseAddrPort(c.PeerListen); err != nil {
			return fmt.Errorf("peer-listen: expected IP:PORT")
		}
		if !membership.Name(c.Cluster) || c.Identity == "" || c.Members == "" {
			return fmt.Errorf("cluster, identity and members are required for registration")
		}
	}
	if c.MaxPeers < 1 || c.MaxPeers > 4096 {
		return fmt.Errorf("max-peers must be between 1 and 4096")
	}
	return nil
}
