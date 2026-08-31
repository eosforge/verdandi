//go:build integration

package verdandi_test

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"os"
	"strings"
	"testing"
	"time"

	verdandi "github.com/LaconisIves/verdandi/sdk/go"
	redis "github.com/redis/go-redis/v9"
)

type rootHashValue struct {
	Revision uint64 `redis:"revision"`
	Name     string `redis:"name"`
	Enabled  bool   `redis:"enabled"`
}

func TestRootCommandsRedisIntegration(t *testing.T) {
	redisURL := strings.TrimSpace(os.Getenv("VERDANDI_REDIS_URL"))
	if redisURL == "" {
		t.Skip("VERDANDI_REDIS_URL is not configured")
	}
	options, err := redis.ParseURL(redisURL)
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	client, err := verdandi.Open(ctx, verdandi.Config{
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
	t.Cleanup(func() {
		if err := client.Close(); err != nil {
			t.Error(err)
		}
	})

	var random [12]byte
	if _, err := rand.Read(random[:]); err != nil {
		t.Fatal(err)
	}
	prefix := "verdandi:test:root:" + hex.EncodeToString(random[:])
	stringKey := prefix + ":string"
	hashKey := prefix + ":hash"
	t.Cleanup(func() {
		_, _ = client.Key().Delete(stringKey)
		_, _ = client.Key().Delete(hashKey)
	})

	if err := client.Ping(); err != nil {
		t.Fatal(err)
	}
	if value, found, err := client.Key().Get[uint64](stringKey); err != nil || found || value != 0 {
		t.Fatalf("unexpected missing typed value: %d, %t, %v", value, found, err)
	}
	if err := client.Key().Store(stringKey, []byte{}); err != nil {
		t.Fatal(err)
	}
	if value, found, err := client.Key().Load(stringKey); err != nil || !found || value == nil || len(value) != 0 {
		t.Fatalf("unexpected empty raw value: %#v, %t, %v", value, found, err)
	}
	if err := client.Key().Set(stringKey, uint64(42)); err != nil {
		t.Fatal(err)
	}
	if value, found, err := client.Key().Get[uint64](stringKey); err != nil || !found || value != 42 {
		t.Fatalf("unexpected typed value: %d, %t, %v", value, found, err)
	}
	if exists, err := client.Key().Exists(stringKey); err != nil || !exists {
		t.Fatalf("unexpected key existence: %t, %v", exists, err)
	}
	if _, err := client.Hash().Load(stringKey); !verdandi.IsCode(err, verdandi.CodeProtocol) {
		t.Fatalf("wrong-type Hash read returned %v, want protocol", err)
	}

	if err := client.Hash().Store(hashKey, verdandi.Fields{"revision": []byte("7"), "name": []byte("north")}); err != nil {
		t.Fatal(err)
	}
	hash, err := client.Hash().Get[rootHashValue](hashKey)
	if err != nil {
		t.Fatal(err)
	}
	if hash.Revision != 7 || hash.Name != "north" || hash.Enabled {
		t.Fatalf("unexpected projected Hash: %#v", hash)
	}
	if err := client.Hash().Set(hashKey, rootHashValue{Revision: 8, Name: "east", Enabled: true}); err != nil {
		t.Fatal(err)
	}
	if fields, err := client.Hash().LoadContext(ctx, hashKey); err != nil || string(fields["enabled"]) != "1" {
		t.Fatalf("unexpected raw Hash: %#v, %v", fields, err)
	}
	if length, err := client.Hash().Length(hashKey); err != nil || length != 3 {
		t.Fatalf("unexpected Hash length: %d, %v", length, err)
	}
	if exists, err := client.Hash().Exists(hashKey, "enabled"); err != nil || !exists {
		t.Fatalf("unexpected field existence: %t, %v", exists, err)
	}
	if deleted, err := client.Hash().Delete(hashKey, "enabled", "missing"); err != nil || deleted != 1 {
		t.Fatalf("unexpected field delete count: %d, %v", deleted, err)
	}

	if err := client.Key().SetWithTTL(stringKey, "expiring", 500*time.Millisecond); err != nil {
		t.Fatal(err)
	}
	if value, found, err := client.Key().Get[string](stringKey); err != nil || !found || value != "expiring" {
		t.Fatalf("unexpected TTL value: %q, %t, %v", value, found, err)
	}
	if applied, err := client.Key().Expire(stringKey, 100*time.Millisecond); err != nil || !applied {
		t.Fatalf("unexpected Expire result: %t, %v", applied, err)
	}
	deadline := time.Now().Add(2 * time.Second)
	for {
		exists, err := client.Key().Exists(stringKey)
		if err != nil {
			t.Fatal(err)
		}
		if !exists {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("expiring key remained present")
		}
		time.Sleep(10 * time.Millisecond)
	}
	if deleted, err := client.Key().Delete(hashKey); err != nil || !deleted {
		t.Fatalf("unexpected complete Hash delete: %t, %v", deleted, err)
	}
	if deleted, err := client.Key().Delete(hashKey); err != nil || deleted {
		t.Fatalf("unexpected repeated delete: %t, %v", deleted, err)
	}
}
