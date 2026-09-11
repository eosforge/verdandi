package registration

import (
	"context"
	"net"
	"testing"
	"time"

	redis "github.com/redis/go-redis/v9"
)

func TestSelectorStartupNotifiesReadyWhenCanceledDuringDial(t *testing.T) {
	owner, cancel := context.WithCancel(context.Background())
	defer cancel()
	client := newTestRuntime(runtimeConfig{selectorSyncTimeout: time.Second}, protocolZoneConfig())
	client.redis = redis.NewClient(&redis.Options{MaxRetries: -1, Dialer: func(context.Context, string, string) (net.Conn, error) {
		cancel()
		return nil, context.Canceled
	}})
	defer client.redis.Close()
	selector := &selectorCore{client: client, release: func() {}, errors: make(chan error, 1), done: make(chan struct{})}
	ready := make(chan error, 1)
	go selector.run(owner, ready)
	select {
	case <-selector.done:
	case <-time.After(2 * time.Second):
		t.Fatal("canceled startup did not terminate")
	}
	select {
	case err := <-ready:
		if !isCode(err, codeClosed) {
			t.Fatalf("startup error = %v, want closed", err)
		}
	default:
		t.Fatal("startup returned without notifying constructor")
	}
}

func TestSelectorRetryFactorOneIgnoresFailureCount(t *testing.T) {
	if delay := selectorRetryDelay(^uint(0), 10*time.Millisecond, time.Second, 1, 0); delay != 10*time.Millisecond {
		t.Fatalf("factor=1 delay = %v", delay)
	}
}
