//go:build integration

package catalog

import (
	"context"
	"fmt"
	"os"
	"strconv"
	"strings"
	"testing"
	"time"

	verdandi "github.com/eosforge/verdandi/sdk/go"
	redis "github.com/redis/go-redis/v9"
)

func TestSharedLuaPatchPreservesAggregateFieldLimit(t *testing.T) {
	endpoint := strings.TrimSpace(os.Getenv("VERDANDI_REDIS_URL"))
	if endpoint == "" {
		t.Skip("VERDANDI_REDIS_URL is not configured")
	}
	options, err := redis.ParseURL(endpoint)
	if err != nil {
		t.Fatal(err)
	}
	raw := redis.NewClient(options)
	defer raw.Close()
	zone := catalogIntegrationZone(t)
	path := Path{part: "limits", id: "fields"}
	keys := mutationKeys(zone, path)
	defer func() {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		if err := raw.Del(ctx, keys...).Err(); err != nil {
			t.Error(err)
		}
	}()
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	eval := func(script string, args ...any) scriptReply {
		t.Helper()
		value, err := raw.Eval(ctx, script, keys, args...).Result()
		if err != nil {
			t.Fatal(err)
		}
		reply, err := parseScriptReply(value)
		if err != nil && !(reply.result == "error" && reply.status == "capacity" && verdandi.IsCode(err, verdandi.CodeCapacity)) {
			t.Fatal(err)
		}
		return reply
	}
	const count = 65_535
	args := []any{path.member(), "map", count * 6, count}
	for index := range count {
		args = append(args, fmt.Sprintf("f%05d", index), "")
	}
	initial := eval(replaceLua, args...)
	if initial.result != "ok" {
		t.Fatalf("initial map rejected: %+v", initial)
	}
	full := eval(patchLua, path.member(), initial.revision, (count+1)*6, 1, "f65535", "")
	if full.result != "ok" {
		t.Fatalf("65,535 to 65,536 fields rejected: %+v", full)
	}
	rejected := eval(patchLua, path.member(), full.revision, (count+2)*6, 1, "f65536", "")
	if rejected.result != "error" || rejected.status != "capacity" || rejected.revision != full.revision {
		t.Fatalf("65,537th field must be rejected atomically: %+v", rejected)
	}
	if raw.HLen(ctx, catalogKey(zone, path)).Val() != 65_536+4 || raw.HGet(ctx, metaKey(zone), "@revision").Val() != strconv.FormatUint(full.revision, 10) ||
		raw.HExists(ctx, catalogKey(zone, path), "f65536").Val() || raw.ZCard(ctx, fieldRevisionsKey(zone, path)).Val() != 65_536 {
		t.Fatal("rejected patch changed stored state or revision")
	}
	overwrite := eval(patchLua, path.member(), full.revision, (count+1)*6+1, 1, "f65535", "x")
	if overwrite.result != "ok" {
		t.Fatalf("existing field update at capacity rejected: %+v", overwrite)
	}
	read, err := raw.Eval(ctx, readLua, readKeys(zone, path), path.member(), 0).Result()
	if err != nil {
		t.Fatal(err)
	}
	state, err := parseReadReply(read, nil, maximumEncodedBytes)
	if err != nil || len(state.fields) != 65_536 || string(state.fields["f65535"]) != "x" {
		t.Fatalf("successful bounded patch must remain readable: %v", err)
	}
}
