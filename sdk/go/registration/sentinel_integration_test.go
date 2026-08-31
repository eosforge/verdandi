//go:build integration

package registration

import (
	"context"
	"os"
	"strings"
	"testing"
	"time"

	verdandi "github.com/LaconisIves/verdandi/sdk/go"
	redis "github.com/redis/go-redis/v9"
)

func TestSentinelRegistrationAndSelectorIntegration(t *testing.T) {
	addressesText := os.Getenv("VERDANDI_SENTINEL_ADDRS")
	if addressesText == "" {
		t.Skip("VERDANDI_SENTINEL_ADDRS is not configured")
	}
	addresses := strings.Split(addressesText, ",")
	sentinel := &Sentinel{
		Addresses:        addresses,
		MasterName:       os.Getenv("VERDANDI_SENTINEL_MASTER"),
		Username:         os.Getenv("VERDANDI_REDIS_USERNAME"),
		Password:         os.Getenv("VERDANDI_REDIS_PASSWORD"),
		SentinelUsername: os.Getenv("VERDANDI_SENTINEL_USERNAME"),
		SentinelPassword: os.Getenv("VERDANDI_SENTINEL_PASSWORD"),
	}
	zone := integrationZone(t)
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	client, err := openTestRegistrationClient(
		t,
		ctx,
		verdandi.Config{Sentinel: sentinel},
		Config{Zone: zone, SelectorPublishInterval: new(time.Millisecond), ClockRefresh: time.Second},
	)
	if err != nil {
		t.Fatal(err)
	}
	selector, err := client.Selector[Fields, Fields](ctx, SelectorOptions{Type: "sentinel"})
	if err != nil {
		t.Fatal(err)
	}
	registration, err := client.Registration[Fields, Fields](RegistrationOptions{
		Type:          "sentinel",
		TTL:           3 * time.Second,
		RenewInterval: 500 * time.Millisecond,
		Version:       1,
	})
	if err != nil {
		t.Fatal(err)
	}
	if err := registration.Register(
		ctx,
		Fields{"language": []byte("go")},
		Fields{"value": []byte("before")},
	); err != nil {
		t.Fatal(err)
	}
	waitForSentinelTypedRecord(t, ctx, selector, registration.UUID(), func(candidate Candidate[Fields, Fields]) bool {
		return string((*candidate.Data)["value"]) == "before"
	})
	if err := registration.Update(ctx, Fields{"value": []byte("after")}); err != nil {
		t.Fatal(err)
	}
	waitForSentinelTypedRecord(t, ctx, selector, registration.UUID(), func(candidate Candidate[Fields, Fields]) bool {
		return candidate.Meta.Revision == 2 && string((*candidate.Data)["value"]) == "after"
	})

	closeCtx, closeCancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer closeCancel()
	if err := registration.Close(closeCtx); err != nil {
		t.Fatal(err)
	}
	if err := selector.Close(closeCtx); err != nil {
		t.Fatal(err)
	}
	if err := client.Close(closeCtx); err != nil {
		t.Fatal(err)
	}
	raw := redis.NewFailoverClient(&redis.FailoverOptions{
		MasterName:       sentinel.MasterName,
		SentinelAddrs:    addresses,
		Username:         sentinel.Username,
		Password:         sentinel.Password,
		SentinelUsername: sentinel.SentinelUsername,
		SentinelPassword: sentinel.SentinelPassword,
	})
	defer func() { _ = raw.Close() }()
	if err := raw.Del(closeCtx, configKey(zone)).Err(); err != nil {
		t.Fatal(err)
	}
}

func waitForSentinelTypedRecord(
	t *testing.T,
	ctx context.Context,
	selector *Selector[Fields, Fields],
	uuid string,
	match func(Candidate[Fields, Fields]) bool,
) {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		candidate, found, err := selector.Find(ctx, uuid)
		if err == nil && found && match(candidate) {
			return
		}
		if err != nil && !IsCode(err, CodeUnavailable) {
			t.Fatal(err)
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatalf("typed Sentinel record %s did not converge", uuid)
}
