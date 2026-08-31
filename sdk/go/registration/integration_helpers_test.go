//go:build integration

package registration

import (
	"context"
	cryptorand "crypto/rand"
	"testing"
	"time"

	verdandi "github.com/LaconisIves/verdandi/sdk/go"
	redis "github.com/redis/go-redis/v9"
)

func integrationZone(t *testing.T) string {
	t.Helper()
	var random [8]byte
	if _, err := cryptorand.Read(random[:]); err != nil {
		t.Fatal(err)
	}
	letters := make([]byte, len(random))
	for index, value := range random {
		letters[index] = 'A' + value%26
	}
	return "GoTest" + string(letters)
}

func cleanupZone(t *testing.T, client *redis.Client, zone string) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	var cursor uint64
	for {
		keys, next, err := client.Scan(ctx, cursor, "verdandi:*:"+zone+":*", 256).Result()
		if err != nil {
			t.Logf("cleanup scan: %v", err)
			return
		}
		if len(keys) > 0 {
			if err := client.Del(ctx, keys...).Err(); err != nil {
				t.Logf("cleanup delete: %v", err)
				return
			}
		}
		cursor = next
		if cursor == 0 {
			break
		}
	}
	_ = client.Del(ctx, configKey(zone)).Err()
}

func requireRegistrationRuntime(t *testing.T, client *Client) *clientRuntime {
	t.Helper()
	runtime, err := runtimeFor(client)
	if err != nil {
		t.Fatal(err)
	}
	return runtime
}

func openTestRegistrationClient(t testing.TB, ctx context.Context, transportConfig verdandi.Config, config Config) (*Client, error) {
	t.Helper()
	transport, err := verdandi.Open(ctx, transportConfig)
	if err != nil {
		return nil, err
	}
	client, err := Open(ctx, transport, config)
	if err != nil {
		_ = transport.Close()
		return nil, err
	}
	t.Cleanup(func() {
		closeCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		_ = client.Close(closeCtx)
		_ = transport.Close()
		cancel()
	})
	return client, nil
}
