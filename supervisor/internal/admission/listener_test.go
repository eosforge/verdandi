package admission

import (
	"errors"
	"net"
	"sync"
	"testing"
	"time"
)

// 模拟 Accept 已获得连接, 但尚未交给 ownedListener 登记时并发发生 Close.
type delayedListener struct {
	conn    net.Conn
	entered chan struct{}
	release chan struct{}
}

func (l *delayedListener) Accept() (net.Conn, error) {
	close(l.entered)
	<-l.release
	return l.conn, nil
}
func (*delayedListener) Close() error   { return nil }
func (*delayedListener) Addr() net.Addr { return &net.TCPAddr{} }

func TestLateAcceptCannotEscapeClosedOwner(t *testing.T) {
	owned, remote := net.Pipe()
	defer remote.Close()
	defer owned.Close()
	base := &delayedListener{conn: owned, entered: make(chan struct{}), release: make(chan struct{})}
	listener := &ownedListener{Listener: base, connections: make(map[*ownedConnection]struct{})}
	result := make(chan error, 1)
	go func() { _, err := listener.Accept(); result <- err }()
	<-base.entered
	if err := listener.Close(); err != nil {
		t.Fatal(err)
	}
	close(base.release)
	select {
	case err := <-result:
		if !errors.Is(err, net.ErrClosed) {
			t.Fatal("late connection escaped close", err)
		}
	case <-time.After(time.Second):
		t.Fatal("late accept blocked")
	}
	if len(listener.connections) != 0 {
		t.Fatal("late connection retained")
	}
}

func TestConcurrentConnectionCloseReturnsPermitOnce(t *testing.T) {
	owned, remote := net.Pipe()
	defer remote.Close()
	listener := &ownedListener{connections: make(map[*ownedConnection]struct{})}
	connection := &ownedConnection{Conn: owned, owner: listener}
	listener.connections[connection] = struct{}{}
	var workers sync.WaitGroup
	for range 16 {
		workers.Go(func() {
			if err := connection.Close(); err != nil {
				t.Error(err)
			}
		})
	}
	workers.Wait()
	if len(listener.connections) != 0 {
		t.Fatal("closed connection retained")
	}
}
