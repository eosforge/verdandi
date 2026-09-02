package verdandi

import (
	"testing"
	"time"

	redis "github.com/redis/go-redis/v9"
)

func TestClientCloseBroadcastsAndIsIdempotent(t *testing.T) {
	client := &Client{
		timeout: time.Second,
		redis:   redis.NewClient(&redis.Options{Addr: "127.0.0.1:1"}),
		done:    make(chan struct{}),
	}
	if client.Redis() == nil || client.Done() == nil || client.Timeout() != time.Second {
		t.Fatal("unexpected transport capability")
	}
	done := client.Done()
	if err := client.Close(); err != nil {
		t.Fatal(err)
	}
	select {
	case <-done:
	default:
		t.Fatal("Close did not broadcast transport shutdown")
	}
	if client.Redis() == nil || client.Done() != done {
		t.Fatal("closed client changed its borrowed transport capability")
	}
	if err := client.Close(); err != nil {
		t.Fatal(err)
	}
}
