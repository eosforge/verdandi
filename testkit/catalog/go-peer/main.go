package main

import (
	"bufio"
	"bytes"
	"context"
	"fmt"
	"os"
	"strconv"
	"strings"
	"time"

	verdandi "github.com/eosforge/verdandi/sdk/go"
	"github.com/eosforge/verdandi/sdk/go/catalog"
	redis "github.com/redis/go-redis/v9"
)

func main() {
	if err := run(); err != nil {
		_, _ = fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

func run() error {
	if len(os.Args) != 2 {
		return fmt.Errorf("usage: catalog-go-peer <zone>")
	}
	transportConfig, config, err := catalogConfig(os.Args[1])
	if err != nil {
		return err
	}
	ctx := context.Background()
	transport, err := verdandi.Open(ctx, transportConfig)
	if err != nil {
		return err
	}
	client, err := catalog.Open(ctx, transport, config)
	if err != nil {
		return err
	}
	publisher, err := client.Publisher()
	if err != nil {
		return err
	}
	path, err := catalog.NewPath("interop", "shared")
	if err != nil {
		return err
	}
	subscriber, err := client.Subscriber(ctx, catalog.Subscription{Paths: []catalog.Path{path}})
	if err != nil {
		return err
	}
	entry := subscriber.Find(path)
	fmt.Println("READY")

	scanner := bufio.NewScanner(os.Stdin)
	for scanner.Scan() {
		fields := strings.Fields(scanner.Text())
		if len(fields) == 0 {
			continue
		}
		switch fields[0] {
		case "REPLACE":
			if len(fields) != 3 {
				return fmt.Errorf("REPLACE requires owner and generation")
			}
			result, operationErr := publisher.Replace(ctx, path, catalog.Map, peerFields(fields[1], fields[2]))
			operationResult(result.Revision, operationErr)
		case "PATCH":
			if len(fields) != 4 {
				return fmt.Errorf("PATCH requires base revision, owner, and generation")
			}
			base, parseErr := strconv.ParseUint(fields[1], 10, 64)
			if parseErr != nil {
				return parseErr
			}
			result, operationErr := publisher.Patch(ctx, path, catalog.Patch{
				BaseRevision: base,
				Set:          peerFields(fields[2], fields[3]),
			})
			operationResult(result.Revision, operationErr)
		case "DELETE":
			result, operationErr := publisher.Delete(ctx, path)
			operationResult(result.Revision, operationErr)
		case "CHECK":
			if len(fields) != 4 {
				return fmt.Errorf("CHECK requires revision, owner, and generation")
			}
			revision, parseErr := strconv.ParseUint(fields[1], 10, 64)
			if parseErr != nil {
				return parseErr
			}
			if checkErr := waitPresent(entry, revision, fields[2], fields[3]); checkErr != nil {
				fmt.Printf("ERROR %v\n", checkErr)
			} else {
				fmt.Println("CHECKED")
			}
		case "CHECK_DELETED":
			if len(fields) != 2 {
				return fmt.Errorf("CHECK_DELETED requires revision")
			}
			revision, parseErr := strconv.ParseUint(fields[1], 10, 64)
			if parseErr != nil {
				return parseErr
			}
			if checkErr := waitDeleted(entry, revision); checkErr != nil {
				fmt.Printf("ERROR %v\n", checkErr)
			} else {
				fmt.Println("CHECKED")
			}
		case "STOP":
			closeCtx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
			defer cancel()
			if err := subscriber.Close(closeCtx); err != nil {
				return err
			}
			if err := client.Close(closeCtx); err != nil {
				return err
			}
			if err := transport.Close(); err != nil {
				return err
			}
			fmt.Println("STOPPED")
			return nil
		default:
			return fmt.Errorf("unknown command %q", fields[0])
		}
	}
	return scanner.Err()
}

func catalogConfig(zone string) (verdandi.Config, catalog.Config, error) {
	transport := verdandi.Config{Timeout: 5 * time.Second}
	config := catalog.Config{Zone: zone, SyncTimeout: 30 * time.Second}
	if addresses := strings.TrimSpace(os.Getenv("VERDANDI_CATALOG_SENTINELS")); addresses != "" {
		transport.Sentinel = &verdandi.Sentinel{
			Addresses:        strings.Split(addresses, ","),
			MasterName:       os.Getenv("VERDANDI_CATALOG_SENTINEL_MASTER"),
			Username:         os.Getenv("VERDANDI_CATALOG_USERNAME"),
			Password:         os.Getenv("VERDANDI_CATALOG_PASSWORD"),
			SentinelUsername: os.Getenv("VERDANDI_CATALOG_SENTINEL_USERNAME"),
			SentinelPassword: os.Getenv("VERDANDI_CATALOG_SENTINEL_PASSWORD"),
		}
		return transport, config, nil
	}
	endpoint := strings.TrimSpace(os.Getenv("VERDANDI_REDIS_URL"))
	options, err := redis.ParseURL(endpoint)
	if err != nil {
		return verdandi.Config{}, catalog.Config{}, fmt.Errorf("parse Redis endpoint: %w", err)
	}
	transport.Standalone = &verdandi.Standalone{
		Address:  options.Addr,
		Username: options.Username,
		Password: options.Password,
		Database: options.DB,
		TLS:      options.TLSConfig,
	}
	return transport, config, nil
}

func peerFields(owner string, generation string) verdandi.Fields {
	return verdandi.Fields{
		"owner":      []byte(owner),
		"generation": []byte(generation),
		"binary":     {0, 255, 1, 254},
	}
}

func operationResult(revision uint64, err error) {
	if err != nil {
		fmt.Printf("ERROR %v\n", err)
		return
	}
	fmt.Printf("REVISION %d\n", revision)
}

func waitPresent(entry *catalog.Entry, revision uint64, owner string, generation string) error {
	deadline := time.Now().Add(30 * time.Second)
	for time.Now().Before(deadline) {
		snapshot, err := entry.Load[verdandi.Fields]()
		if err != nil {
			return err
		}
		if snapshot.Synchronized && snapshot.Status == catalog.StatusPresent &&
			snapshot.Revision == revision && snapshot.Value != nil &&
			string((*snapshot.Value)["owner"]) == owner &&
			string((*snapshot.Value)["generation"]) == generation &&
			bytes.Equal((*snapshot.Value)["binary"], []byte{0, 255, 1, 254}) {
			return nil
		}
		time.Sleep(10 * time.Millisecond)
	}
	return fmt.Errorf("Catalog value did not converge to revision %d", revision)
}

func waitDeleted(entry *catalog.Entry, revision uint64) error {
	deadline := time.Now().Add(30 * time.Second)
	for time.Now().Before(deadline) {
		snapshot, err := entry.Load[verdandi.Fields]()
		if err != nil {
			return err
		}
		if snapshot.Synchronized && snapshot.Status == catalog.StatusDeleted &&
			snapshot.Revision == revision && snapshot.Value == nil {
			return nil
		}
		time.Sleep(10 * time.Millisecond)
	}
	return fmt.Errorf("Catalog deletion did not converge to revision %d", revision)
}
