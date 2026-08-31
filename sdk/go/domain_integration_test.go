//go:build integration

package verdandi_test

import (
	"context"
	cryptorand "crypto/rand"
	"os"
	"testing"
	"time"

	verdandi "github.com/LaconisIves/verdandi/sdk/go"
	"github.com/LaconisIves/verdandi/sdk/go/registration"
	redis "github.com/redis/go-redis/v9"
)

func TestSharedTransportSupportsIndependentZonesAndBroadcastShutdown(t *testing.T) {
	redisURL := os.Getenv("VERDANDI_REDIS_URL")
	if redisURL == "" {
		t.Skip("VERDANDI_REDIS_URL is not configured")
	}
	options, err := redis.ParseURL(redisURL)
	if err != nil {
		t.Fatal(err)
	}
	raw := redis.NewClient(options)
	t.Cleanup(func() { _ = raw.Close() })
	zoneA := transportTestZone(t)
	zoneB := transportTestZone(t)
	t.Cleanup(func() {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = raw.Del(ctx, "verdandi:config:"+zoneA, "verdandi:config:"+zoneB).Err()
	})

	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	transport, err := verdandi.Open(ctx, verdandi.Config{
		Standalone: &verdandi.Standalone{
			Address:  options.Addr,
			Username: options.Username,
			Password: options.Password,
			Database: options.DB,
			TLS:      options.TLSConfig,
		},
	})
	if err != nil {
		t.Fatal(err)
	}
	first, err := registration.Open(ctx, transport, registration.Config{Zone: zoneA})
	if err != nil {
		_ = transport.Close()
		t.Fatal(err)
	}
	second, err := registration.Open(ctx, transport, registration.Config{Zone: zoneB})
	if err != nil {
		closeCtx, closeCancel := context.WithTimeout(context.Background(), 5*time.Second)
		_ = first.Close(closeCtx)
		closeCancel()
		_ = transport.Close()
		t.Fatal(err)
	}
	t.Cleanup(func() {
		closeCtx, closeCancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer closeCancel()
		_ = first.Close(closeCtx)
		_ = second.Close(closeCtx)
		_ = transport.Close()
	})

	exists, err := raw.Exists(ctx, "verdandi:config:"+zoneA, "verdandi:config:"+zoneB).Result()
	if err != nil {
		t.Fatal(err)
	}
	if exists != 2 {
		t.Fatalf("shared transport initialized %d Zone configurations, want 2", exists)
	}
	if err := transport.Close(); err != nil {
		t.Fatal(err)
	}
	if _, err := registration.Open(ctx, transport, registration.Config{Zone: transportTestZone(t)}); !verdandi.IsCode(err, verdandi.CodeClosed) {
		t.Fatalf("Registration Open after transport Close = %v, want closed", err)
	}
	closeCtx, closeCancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer closeCancel()
	if err := first.Close(closeCtx); err != nil {
		t.Fatal(err)
	}
	if err := second.Close(closeCtx); err != nil {
		t.Fatal(err)
	}
}

func transportTestZone(t *testing.T) string {
	t.Helper()
	var random [8]byte
	if _, err := cryptorand.Read(random[:]); err != nil {
		t.Fatal(err)
	}
	zone := make([]byte, len("GoTransport")+len(random))
	copy(zone, "GoTransport")
	for index, value := range random {
		zone[len("GoTransport")+index] = 'A' + value%26
	}
	return string(zone)
}
