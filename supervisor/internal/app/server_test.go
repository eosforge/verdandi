package app

import (
	"bufio"
	"context"
	"errors"
	"io"
	"log/slog"
	"net"
	"net/http"
	"strings"
	"sync"
	"testing"
	"time"

	"golang.org/x/net/netutil"

	"github.com/eosforge/verdandi/supervisor/internal/management"
)

type observedListener struct {
	net.Listener
	closed chan struct{}
	once   sync.Once
}

func (l *observedListener) Close() error {
	err := l.Listener.Close()
	l.once.Do(func() { close(l.closed) })
	return err
}

type runningServer struct {
	address        string
	cancel         context.CancelFunc
	done           chan struct{}
	listenerClosed <-chan struct{}
	err            error
}

func startServer(t *testing.T, cfg Config, handler http.Handler) *runningServer {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	observed := &observedListener{Listener: listener, closed: make(chan struct{})}
	ctx, cancel := context.WithCancel(context.Background())
	running := &runningServer{address: listener.Addr().String(), cancel: cancel, done: make(chan struct{}), listenerClosed: observed.closed}
	go func() {
		running.err = serve(ctx, cfg, slog.New(slog.NewTextHandler(io.Discard, nil)), observed, handler)
		close(running.done)
	}()
	t.Cleanup(func() {
		cancel()
		await(t, running.done)
	})
	return running
}

func await(t *testing.T, done <-chan struct{}) {
	t.Helper()
	select {
	case <-done:
	case <-time.After(3 * time.Second):
		t.Fatal("operation did not finish")
	}
}

func TestManagementHTTPAndPortCleanup(t *testing.T) {
	running := startServer(t, DefaultConfig(), management.Handler())
	transport := &http.Transport{}
	t.Cleanup(transport.CloseIdleConnections)
	client := &http.Client{Transport: transport, Timeout: time.Second}
	for _, tc := range []struct {
		method, path, body string
		status             int
	}{
		{"GET", "/healthz", "", 200},
		{"HEAD", "/healthz", "", 200},
		{"POST", "/healthz", "", 405},
		{"GET", "/stars", "", 404},
		{"GET", "/healthz", "unexpected", 400},
	} {
		t.Run(tc.method+tc.path+tc.body, func(t *testing.T) {
			request, err := http.NewRequestWithContext(t.Context(), tc.method, "http://"+running.address+tc.path, strings.NewReader(tc.body))
			if err != nil {
				t.Fatal(err)
			}
			response, err := client.Do(request)
			if err != nil {
				t.Fatal(err)
			}
			defer response.Body.Close()
			body, err := io.ReadAll(response.Body)
			if err != nil {
				t.Fatal(err)
			}
			if response.StatusCode != tc.status {
				t.Fatalf("status = %d, want %d: %s", response.StatusCode, tc.status, body)
			}
			if tc.method == "GET" && tc.status == 200 && !strings.Contains(string(body), `"scope":"management"`) {
				t.Fatalf("unexpected health scope: %s", body)
			}
		})
	}
	running.cancel()
	await(t, running.done)
	if running.err != nil {
		t.Fatal(running.err)
	}
	listener, err := net.Listen("tcp", running.address)
	if err != nil {
		t.Fatalf("port was not released: %v", err)
	}
	listener.Close()
}

func TestShutdownDrainsActiveRequest(t *testing.T) {
	entered, finish := make(chan struct{}), make(chan struct{})
	running := startServer(t, DefaultConfig(), http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		close(entered)
		select {
		case <-finish:
			_, _ = io.WriteString(w, "finished")
		case <-r.Context().Done():
		}
	}))
	result := make(chan error, 1)
	go func() {
		client := &http.Client{Transport: &http.Transport{DisableKeepAlives: true}, Timeout: 2 * time.Second}
		response, err := client.Get("http://" + running.address)
		if err == nil {
			var body []byte
			body, err = io.ReadAll(response.Body)
			response.Body.Close()
			if err == nil && string(body) != "finished" {
				err = errors.New("active request did not finish")
			}
		}
		result <- err
	}()
	await(t, entered)
	running.cancel()
	await(t, running.listenerClosed)
	close(finish)
	if err := <-result; err != nil {
		t.Fatal(err)
	}
	await(t, running.done)
	if running.err != nil {
		t.Fatal(running.err)
	}
}

func TestShutdownDeadlineCancelsActiveRequest(t *testing.T) {
	cfg := DefaultConfig()
	cfg.ShutdownTimeout = 50 * time.Millisecond
	entered, canceled := make(chan struct{}), make(chan struct{})
	running := startServer(t, cfg, http.HandlerFunc(func(_ http.ResponseWriter, r *http.Request) {
		close(entered)
		<-r.Context().Done()
		close(canceled)
	}))
	requestDone := make(chan struct{})
	go func() {
		client := &http.Client{Transport: &http.Transport{DisableKeepAlives: true}, Timeout: 2 * time.Second}
		response, err := client.Get("http://" + running.address)
		if err == nil {
			response.Body.Close()
		}
		close(requestDone)
	}()
	await(t, entered)
	running.cancel()
	await(t, canceled)
	await(t, requestDone)
	await(t, running.done)
	if !errors.Is(running.err, context.DeadlineExceeded) {
		t.Fatalf("shutdown error = %v, want deadline exceeded", running.err)
	}
}

func TestConnectionLimitResumesAfterIdleConnectionCloses(t *testing.T) {
	cfg := DefaultConfig()
	cfg.MaxConnections = 1
	running := startServer(t, cfg, management.Handler())
	transport := &http.Transport{}
	defer transport.CloseIdleConnections()
	client := &http.Client{Transport: transport, Timeout: time.Second}
	response, err := client.Get("http://" + running.address + "/healthz")
	if err != nil {
		t.Fatal(err)
	}
	_, err = io.Copy(io.Discard, response.Body)
	response.Body.Close()
	if err != nil {
		t.Fatal(err)
	}

	// 第一条连接空闲保留, 第二条 TCP 可进入系统 backlog, 但不能被 HTTP 服务处理.
	queued, err := net.DialTimeout("tcp", running.address, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	defer queued.Close()
	if err := queued.SetDeadline(time.Now().Add(time.Second)); err != nil {
		t.Fatal(err)
	}
	if _, err := io.WriteString(queued, "GET /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"); err != nil {
		t.Fatal(err)
	}
	if err := queued.SetReadDeadline(time.Now().Add(50 * time.Millisecond)); err != nil {
		t.Fatal(err)
	}
	var firstByte [1]byte
	n, err := queued.Read(firstByte[:])
	var timeout net.Error
	if n != 0 || !errors.As(err, &timeout) || !timeout.Timeout() {
		t.Fatalf("connection limit did not block HTTP: bytes=%d, error=%v", n, err)
	}

	// 关闭空闲连接归还名额后, 同一条排队连接应该正常完成请求.
	transport.CloseIdleConnections()
	if err := queued.SetReadDeadline(time.Now().Add(time.Second)); err != nil {
		t.Fatal(err)
	}
	resumed, err := http.ReadResponse(bufio.NewReader(queued), nil)
	if err != nil {
		t.Fatal(err)
	}
	defer resumed.Body.Close()
	if resumed.StatusCode != http.StatusOK {
		t.Fatalf("resumed HTTP status = %d", resumed.StatusCode)
	}
}

func TestLimitedListenerCloseUnblocksFullCapacity(t *testing.T) {
	raw, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	listener := netutil.LimitListener(raw, 1)
	defer listener.Close()
	client, err := net.DialTimeout("tcp", raw.Addr().String(), time.Second)
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()
	conn, err := listener.Accept()
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	result := make(chan error, 1)
	go func() {
		_, err := listener.Accept()
		result <- err
	}()
	listener.Close()
	select {
	case err := <-result:
		if !errors.Is(err, net.ErrClosed) {
			t.Fatalf("accept after close: %v", err)
		}
	case <-time.After(time.Second):
		t.Fatal("Accept remained blocked at capacity")
	}
	conn.Close()
	conn.Close()
}

func TestRunReportsOccupiedPort(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	cfg := DefaultConfig()
	cfg.Listen = listener.Addr().String()
	if err := Run(t.Context(), cfg, slog.New(slog.NewTextHandler(io.Discard, nil))); err == nil {
		t.Fatal("occupied port was accepted")
	}
}

func TestServeReportsListenerFailureWithoutCancellation(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	listener.Close()
	ctx, cancel := context.WithTimeout(t.Context(), time.Second)
	defer cancel()
	err = serve(ctx, DefaultConfig(), slog.New(slog.NewTextHandler(io.Discard, nil)), listener, management.Handler())
	if err == nil || ctx.Err() != nil {
		t.Fatalf("listener failure should propagate before context cancellation: error=%v, context=%v", err, ctx.Err())
	}
}
