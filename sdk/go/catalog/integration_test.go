//go:build integration

package catalog

import (
	"bytes"
	"context"
	"crypto/rand"
	"errors"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"

	verdandi "github.com/eosforge/verdandi/sdk/go"
	redis "github.com/redis/go-redis/v9"
)

type integrationValue struct {
	Text string
}

func (value integrationValue) Encode() (verdandi.Fields, error) {
	return verdandi.Fields{"value": []byte(value.Text)}, nil
}

func (value *integrationValue) Decode(source verdandi.Fields) error {
	value.Text = string(source["value"])
	return nil
}

func TestCatalogPublisherSubscriberIntegration(t *testing.T) {
	redisURL := strings.TrimSpace(os.Getenv("VERDANDI_REDIS_URL"))
	if redisURL == "" {
		t.Skip("VERDANDI_REDIS_URL is not configured")
	}
	options, err := redis.ParseURL(redisURL)
	if err != nil {
		t.Fatal(err)
	}
	raw := redis.NewClient(options)
	t.Cleanup(func() { _ = raw.Close() })
	zone := catalogIntegrationZone(t)
	t.Cleanup(func() { cleanupCatalogIntegrationZone(t, raw, zone) })

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
	t.Cleanup(func() {
		if err := transport.Close(); err != nil {
			t.Error(err)
		}
	})
	client, err := Open(ctx, transport, Config{Zone: zone, LocalStorePath: filepath.Join(t.TempDir(), "catalog.db")})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		closeCtx, closeCancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer closeCancel()
		if err := client.Close(closeCtx); err != nil {
			t.Error(err)
		}
	})
	publisher, err := client.Publisher()
	if err != nil {
		t.Fatal(err)
	}
	routePath, err := NewPath("routing", "public")
	if err != nil {
		t.Fatal(err)
	}
	arrayPath, err := NewPath("routing", "backends")
	if err != nil {
		t.Fatal(err)
	}
	valuePath, err := NewPath("routing", "banner")
	if err != nil {
		t.Fatal(err)
	}
	subscriber, err := client.Subscriber(ctx, Subscription{Parts: []string{"routing"}})
	if err != nil {
		t.Fatal(err)
	}
	waitCatalogWorkerCount(t, subscriber, 1)
	t.Cleanup(func() {
		closeCtx, closeCancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer closeCancel()
		if err := subscriber.Close(closeCtx); err != nil {
			t.Error(err)
		}
	})

	routeEntry := subscriber.Find(routePath)
	if routeEntry == nil || routeEntry.Status() != StatusAbsent {
		t.Fatalf("unexpected initial route state: %#v", routeEntry)
	}
	created, err := publisher.Replace(ctx, routePath, Map, verdandi.Fields{
		"primary": []byte("east"),
		"weight":  []byte("10"),
	})
	if err != nil {
		t.Fatal(err)
	}
	waitCatalogState(t, routeEntry, StatusPresent, created.Revision)
	rawRoute, err := routeEntry.Load[verdandi.Fields]()
	if err != nil {
		t.Fatal(err)
	}
	if rawRoute.Value == nil || !bytes.Equal((*rawRoute.Value)["primary"], []byte("east")) {
		t.Fatalf("unexpected route snapshot: %#v", rawRoute)
	}

	// Force a local Pub/Sub baseline miss. The Patch notification cannot apply
	// directly and must trigger an authoritative field-version repair.
	routeEntry.state.Store(initialState(StatusAbsent))
	patched, err := publisher.Patch(ctx, routePath, Patch{
		BaseRevision: created.Revision,
		Set: verdandi.Fields{
			"primary": []byte("west"),
			"zone":    []byte("secondary"),
		},
	})
	if err != nil {
		t.Fatal(err)
	}
	waitCatalogState(t, routeEntry, StatusPresent, patched.Revision)
	waitCatalogWorkerCount(t, subscriber, 1)
	_, err = publisher.Patch(ctx, routePath, Patch{
		BaseRevision: created.Revision,
		Set:          verdandi.Fields{"primary": []byte("stale")},
	})
	var stale *verdandi.Error
	if !errors.As(err, &stale) || stale.Code != verdandi.CodeStale ||
		stale.Revision != patched.Revision {
		t.Fatalf("unexpected stale Patch result: %#v", err)
	}
	rawRoute, err = routeEntry.Load[verdandi.Fields]()
	if err != nil {
		t.Fatal(err)
	}
	if rawRoute.Value == nil || !bytes.Equal((*rawRoute.Value)["primary"], []byte("west")) ||
		!bytes.Equal((*rawRoute.Value)["zone"], []byte("secondary")) {
		t.Fatalf("repair did not reconstruct patch: %#v", rawRoute)
	}

	array, err := publisher.Replace(ctx, arrayPath, Array, verdandi.Fields{
		"0": []byte("one"),
		"1": []byte("two"),
	})
	if err != nil {
		t.Fatal(err)
	}
	arrayEntry := subscriber.Find(arrayPath)
	waitCatalogState(t, arrayEntry, StatusPresent, array.Revision)
	arrayPatch, err := publisher.Patch(ctx, arrayPath, Patch{
		BaseRevision: array.Revision,
		Set:          verdandi.Fields{"1": []byte("changed")},
	})
	if err != nil {
		t.Fatal(err)
	}
	waitCatalogState(t, arrayEntry, StatusPresent, arrayPatch.Revision)
	if _, err := publisher.Patch(ctx, arrayPath, Patch{
		BaseRevision: arrayPatch.Revision,
		Set:          verdandi.Fields{"2": []byte("append")},
	}); !verdandi.IsCode(err, verdandi.CodeTransition) {
		t.Fatalf("expected array append rejection, got %v", err)
	}

	value, err := publisher.Replace(ctx, valuePath, Value, integrationValue{Text: "ready"})
	if err != nil {
		t.Fatal(err)
	}
	valueEntry := subscriber.Find(valuePath)
	waitCatalogState(t, valueEntry, StatusPresent, value.Revision)
	typed, err := valueEntry.Load[integrationValue]()
	if err != nil {
		t.Fatal(err)
	}
	if typed.Value == nil || typed.Value.Text != "ready" {
		t.Fatalf("unexpected typed value: %#v", typed)
	}

	deleted, err := publisher.Delete(ctx, routePath)
	if err != nil {
		t.Fatal(err)
	}
	waitCatalogState(t, routeEntry, StatusDeleted, deleted.Revision)
	if subscriber.Find(routePath) != routeEntry {
		t.Fatal("delete replaced the stable Entry")
	}

	connectionName := "catalog-integration-" + zone
	if err := subscriber.pubsub.ClientSetName(ctx, connectionName); err != nil {
		t.Fatal(err)
	}
	connectionID := catalogClientID(t, ctx, raw, connectionName)
	if killed, err := raw.ClientKillByFilter(ctx, "ID", connectionID).Result(); err != nil || killed != 1 {
		t.Fatalf("kill Catalog Pub/Sub connection: killed=%d err=%v", killed, err)
	}
	recreated, err := publisher.Replace(ctx, routePath, Map, verdandi.Fields{
		"primary": []byte("north"),
	})
	if err != nil {
		t.Fatal(err)
	}
	waitCatalogState(t, routeEntry, StatusPresent, recreated.Revision)
	waitCatalogWorkerCount(t, subscriber, 1)
	rawRoute, err = routeEntry.Load[verdandi.Fields]()
	if err != nil {
		t.Fatal(err)
	}
	if rawRoute.Value == nil || !bytes.Equal((*rawRoute.Value)["primary"], []byte("north")) {
		t.Fatalf("reconnect repair did not converge: %#v", rawRoute)
	}

	closeCtx, closeCancel := context.WithTimeout(context.Background(), 5*time.Second)
	if err := subscriber.Close(closeCtx); err != nil {
		closeCancel()
		t.Fatal(err)
	}
	closeCancel()
	if workers := subscriber.workers.Load(); workers != 0 {
		t.Fatalf("closed Subscriber retained %d workers", workers)
	}
	if subscriber.Find(valuePath) != nil {
		t.Fatal("closed Subscriber returned an Entry")
	}
	closedValue, err := valueEntry.Load[integrationValue]()
	if err != nil {
		t.Fatal(err)
	}
	if closedValue.Status != StatusClosed || closedValue.Synchronized ||
		closedValue.Value == nil || closedValue.Value.Text != "ready" {
		t.Fatalf("closed Entry did not retain its last complete value: %#v", closedValue)
	}
	// A fresh full alignment advances the persisted scope cursor. The next
	// generation must then recover the offline Patch through the Zone ZSET delta.
	subscriber, err = client.Subscriber(ctx, Subscription{Parts: []string{"routing"}})
	if err != nil {
		t.Fatal(err)
	}
	routeEntry = subscriber.Find(routePath)
	waitCatalogState(t, routeEntry, StatusPresent, recreated.Revision)
	closeCtx, closeCancel = context.WithTimeout(context.Background(), 5*time.Second)
	if err := subscriber.Close(closeCtx); err != nil {
		closeCancel()
		t.Fatal(err)
	}
	closeCancel()
	offline, err := publisher.Patch(ctx, routePath, Patch{
		BaseRevision: recreated.Revision,
		Set:          verdandi.Fields{"primary": []byte("offline")},
	})
	if err != nil {
		t.Fatal(err)
	}
	subscriber, err = client.Subscriber(ctx, Subscription{Parts: []string{"routing"}})
	if err != nil {
		t.Fatal(err)
	}
	routeEntry = subscriber.Find(routePath)
	waitCatalogState(t, routeEntry, StatusPresent, offline.Revision)
	rawRoute, err = routeEntry.Load[verdandi.Fields]()
	if err != nil {
		t.Fatal(err)
	}
	if rawRoute.Value == nil || !bytes.Equal((*rawRoute.Value)["primary"], []byte("offline")) {
		t.Fatalf("checkpoint delta did not converge: %#v", rawRoute)
	}

	// Evict a tombstone and advance the floor past the persisted cursor. The
	// next generation must reject the incomplete ZSET delta and full-align.
	closeCtx, closeCancel = context.WithTimeout(context.Background(), 5*time.Second)
	if err := subscriber.Close(closeCtx); err != nil {
		closeCancel()
		t.Fatal(err)
	}
	closeCancel()
	temporaryPath, err := NewPath("routing", "evicted")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := publisher.Replace(ctx, temporaryPath, Map, verdandi.Fields{
		"target": []byte("temporary"),
	}); err != nil {
		t.Fatal(err)
	}
	evicted, err := publisher.Delete(ctx, temporaryPath)
	if err != nil {
		t.Fatal(err)
	}
	if err := raw.ZRem(ctx, deletedKey(zone), temporaryPath.member()).Err(); err != nil {
		t.Fatal(err)
	}
	if err := raw.ZRem(ctx, deletedTimeKey(zone), temporaryPath.member()).Err(); err != nil {
		t.Fatal(err)
	}
	if err := raw.HSet(ctx, metaKey(zone), "@floor_revision", evicted.Revision).Err(); err != nil {
		t.Fatal(err)
	}
	current, err := publisher.Patch(ctx, routePath, Patch{
		BaseRevision: offline.Revision,
		Set:          verdandi.Fields{"primary": []byte("full-alignment")},
	})
	if err != nil {
		t.Fatal(err)
	}
	subscriber, err = client.Subscriber(ctx, Subscription{Parts: []string{"routing"}})
	if err != nil {
		t.Fatal(err)
	}
	routeEntry = subscriber.Find(routePath)
	waitCatalogState(t, routeEntry, StatusPresent, current.Revision)
	rawRoute, err = routeEntry.Load[verdandi.Fields]()
	if err != nil {
		t.Fatal(err)
	}
	if rawRoute.Value == nil ||
		!bytes.Equal((*rawRoute.Value)["primary"], []byte("full-alignment")) {
		t.Fatalf("below-floor full alignment did not converge: %#v", rawRoute)
	}
	temporaryEntry := subscriber.Find(temporaryPath)
	if temporaryEntry == nil || temporaryEntry.Status() != StatusAbsent {
		t.Fatalf("evicted tombstone survived full alignment: %#v", temporaryEntry)
	}
}

func waitCatalogState(t *testing.T, entry *Entry, status Status, revision uint64) {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if entry != nil && entry.Status() == status && entry.Revision() == revision {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	if entry == nil {
		t.Fatal("Catalog Entry is nil")
	}
	t.Fatalf("Catalog state did not converge: status=%d revision=%d", entry.Status(), entry.Revision())
}

func waitCatalogWorkerCount(t *testing.T, subscriber *Subscriber, expected int32) {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if subscriber.workers.Load() == expected {
			return
		}
		time.Sleep(time.Millisecond)
	}
	t.Fatalf("Catalog worker count did not converge: got=%d want=%d", subscriber.workers.Load(), expected)
}

func catalogIntegrationZone(t *testing.T) string {
	t.Helper()
	random := make([]byte, 12)
	if _, err := rand.Read(random); err != nil {
		t.Fatal(err)
	}
	for index := range random {
		random[index] = 'a' + random[index]%26
	}
	return "Catalog" + string(random)
}

func catalogClientID(t *testing.T, ctx context.Context, client *redis.Client, name string) string {
	t.Helper()
	listing, err := client.ClientList(ctx).Result()
	if err != nil {
		t.Fatal(err)
	}
	for _, line := range strings.Split(listing, "\n") {
		fields := make(map[string]string)
		for _, item := range strings.Fields(line) {
			key, value, found := strings.Cut(item, "=")
			if found {
				fields[key] = value
			}
		}
		if fields["name"] == name {
			if _, err := strconv.ParseUint(fields["id"], 10, 64); err != nil {
				t.Fatalf("invalid client id %q", fields["id"])
			}
			return fields["id"]
		}
	}
	t.Fatalf("Pub/Sub client %q not found", name)
	return ""
}

func cleanupCatalogIntegrationZone(t *testing.T, client *redis.Client, zone string) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	var cursor uint64
	for {
		keys, next, err := client.Scan(ctx, cursor, zonePrefix(zone)+"*", 256).Result()
		if err != nil {
			t.Error(err)
			return
		}
		if len(keys) != 0 {
			if err := client.Unlink(ctx, keys...).Err(); err != nil {
				t.Error(err)
				return
			}
		}
		cursor = next
		if cursor == 0 {
			return
		}
	}
}
