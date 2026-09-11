package admission

import (
	"net"
	"sync"
)

// ownedListener 跟踪尚未完成 TLS 的原始连接. gRPC Stop 只拥有已建立的 transport,
// 所以停机必须先关闭这些 socket, 再等待框架和 handler 退出.
type ownedListener struct {
	net.Listener
	mutex       sync.Mutex
	connections map[*ownedConnection]struct{}
	closed      bool
}
type ownedConnection struct {
	net.Conn
	owner *ownedListener
	once  sync.Once
	err   error
}

// Accept 先取得底层名额, 再登记所有权. Close 与 Accept 交错时直接关闭迟到连接.
func (l *ownedListener) Accept() (net.Conn, error) {
	raw, err := l.Listener.Accept()
	if err != nil {
		return nil, err
	}
	l.mutex.Lock()
	defer l.mutex.Unlock()
	if l.closed {
		_ = raw.Close()
		return nil, net.ErrClosed
	}
	connection := &ownedConnection{Conn: raw, owner: l}
	l.connections[connection] = struct{}{}
	return connection, nil
}

// Close 先封闭新增登记, 再在锁外关闭 socket, 防止 Close 回调归还名额时重入同一把锁.
func (l *ownedListener) Close() error {
	l.mutex.Lock()
	l.closed = true
	connections := make([]*ownedConnection, 0, len(l.connections))
	for connection := range l.connections {
		connections = append(connections, connection)
	}
	l.mutex.Unlock()
	err := l.Listener.Close()
	for _, connection := range connections {
		_ = connection.Close()
	}
	return err
}

// Close 可并发重复调用, 底层连接和连接名额恰好释放一次, 返回同一个关闭结果.
func (c *ownedConnection) Close() error {
	c.once.Do(func() {
		c.err = c.Conn.Close()
		c.owner.mutex.Lock()
		delete(c.owner.connections, c)
		c.owner.mutex.Unlock()
	})
	return c.err
}
