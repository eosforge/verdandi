package main

import (
	"bufio"
	"bytes"
	"context"
	"fmt"
	"os"
	"strings"
	"time"

	verdandi "github.com/eosforge/verdandi/sdk/go"
	"github.com/eosforge/verdandi/sdk/go/catalog"
	discovery "github.com/eosforge/verdandi/sdk/go/registration"
	redis "github.com/redis/go-redis/v9"
)

func main() {
	if err := run(); err != nil {
		_, _ = fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

func run() error {
	if len(os.Args) != 3 {
		return fmt.Errorf("usage: go-peer <redis-url> <zone>")
	}
	config, err := redisConfig(os.Args[1])
	if err != nil {
		return err
	}
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()
	client, err := verdandi.Open(ctx, config)
	if err != nil {
		return err
	}
	registrationClient, err := discovery.Open(ctx, client, discovery.Config{
		Zone:                    os.Args[2],
		SelectorPublishInterval: new(time.Millisecond),
		ClockRefresh:            time.Second,
	})
	if err != nil {
		return err
	}
	selector, err := registrationClient.Selector[verdandi.Fields, verdandi.Fields](
		ctx,
		discovery.SelectorOptions{Type: "interop"},
	)
	if err != nil {
		return err
	}
	catalogClient, err := catalog.Open(ctx, client, catalog.Config{Zone: os.Args[2]})
	if err != nil {
		return err
	}
	catalogPublisher, err := catalogClient.Publisher()
	if err != nil {
		return err
	}
	catalogPath, err := catalog.NewPath("interop", "shared")
	if err != nil {
		return err
	}
	catalogSubscriber, err := catalogClient.Subscriber(ctx, catalog.Subscription{
		Paths: []catalog.Path{catalogPath},
	})
	if err != nil {
		return err
	}
	catalogEntry := catalogSubscriber.Find(catalogPath)
	fmt.Println("READY")

	var registration *discovery.Registration[verdandi.Fields, verdandi.Fields]
	scanner := bufio.NewScanner(os.Stdin)
	for scanner.Scan() {
		fields := strings.Fields(scanner.Text())
		if len(fields) == 0 {
			continue
		}
		switch fields[0] {
		case "PRODUCE":
			if registration != nil {
				return fmt.Errorf("Go Registration already exists")
			}
			registration, err = registrationClient.Registration[verdandi.Fields, verdandi.Fields](discovery.RegistrationOptions{
				Type:          "interop",
				TTL:           30 * time.Second,
				RenewInterval: 5 * time.Second,
				Version:       11,
			})
			if err != nil {
				return err
			}
			if err := registration.Register(
				ctx,
				verdandi.Fields{"language": []byte("go")},
				verdandi.Fields{"payload": {0, 255, 1}},
			); err != nil {
				return err
			}
			if err := registration.UpdateContent(ctx, 12, verdandi.Fields{"payload": {0, 255, 1, 2}}); err != nil {
				return err
			}
			fmt.Printf("GO %s\n", registration.UUID())
		case "VERIFY_RUST":
			if len(fields) != 2 {
				return fmt.Errorf("VERIFY_RUST requires UUID")
			}
			if err := waitRecord(selector, fields[1], func(record discovery.Candidate[verdandi.Fields, verdandi.Fields]) bool {
				return record.Meta.Revision == 2 && record.Meta.Version == 22 &&
					string((*record.Attr)["language"]) == "rust" &&
					bytes.Equal((*record.Data)["payload"], []byte{255, 0, 2, 3})
			}); err != nil {
				return err
			}
			fmt.Println("VERIFIED_RUST")
		case "VERIFY_GO":
			if len(fields) != 2 {
				return fmt.Errorf("VERIFY_GO requires UUID")
			}
			if err := waitRecord(selector, fields[1], func(record discovery.Candidate[verdandi.Fields, verdandi.Fields]) bool {
				return record.Meta.Revision == 2 && record.Meta.Version == 12 &&
					string((*record.Attr)["language"]) == "go" &&
					bytes.Equal((*record.Data)["payload"], []byte{0, 255, 1, 2})
			}); err != nil {
				return err
			}
			fmt.Println("VERIFIED_GO")
		case "CATALOG_PRODUCE":
			result, replaceErr := catalogPublisher.Replace(
				ctx,
				catalogPath,
				catalog.Map,
				verdandi.Fields{
					"go":     {0, 255, 1},
					"shared": []byte("go"),
				},
			)
			if replaceErr != nil {
				return replaceErr
			}
			fmt.Printf("CATALOG_GO %d\n", result.Revision)
		case "VERIFY_CATALOG_RUST":
			if err := waitCatalog(catalogEntry, func(snapshot catalog.Snapshot[verdandi.Fields]) bool {
				return snapshot.Synchronized && snapshot.Revision == 2 &&
					snapshot.Status == catalog.StatusPresent && snapshot.Value != nil &&
					bytes.Equal((*snapshot.Value)["rust"], []byte{255, 0, 2}) &&
					string((*snapshot.Value)["shared"]) == "rust" && (*snapshot.Value)["go"] == nil
			}); err != nil {
				return err
			}
			fmt.Println("VERIFIED_CATALOG_RUST")
		case "CATALOG_DELETE":
			result, deleteErr := catalogPublisher.Delete(ctx, catalogPath)
			if deleteErr != nil {
				return deleteErr
			}
			fmt.Printf("CATALOG_DELETED %d\n", result.Revision)
		case "STOP":
			closeCtx, closeCancel := context.WithTimeout(context.Background(), 10*time.Second)
			defer closeCancel()
			if registration != nil {
				if err := registration.Close(closeCtx); err != nil {
					return err
				}
			}
			if err := selector.Close(closeCtx); err != nil {
				return err
			}
			if err := catalogSubscriber.Close(closeCtx); err != nil {
				return err
			}
			if err := catalogClient.Close(closeCtx); err != nil {
				return err
			}
			if err := registrationClient.Close(closeCtx); err != nil {
				return err
			}
			if err := client.Close(); err != nil {
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

func redisConfig(endpoint string) (verdandi.Config, error) {
	options, err := redis.ParseURL(endpoint)
	if err != nil {
		return verdandi.Config{}, fmt.Errorf("parse Redis endpoint: %w", err)
	}
	return verdandi.Config{
		Standalone: &verdandi.Standalone{
			Address:  options.Addr,
			Username: options.Username,
			Password: options.Password,
			Database: options.DB,
			TLS:      options.TLSConfig,
		},
		Timeout: 5 * time.Second,
	}, nil
}

func waitRecord(
	selector *discovery.Selector[verdandi.Fields, verdandi.Fields],
	uuid string,
	predicate func(discovery.Candidate[verdandi.Fields, verdandi.Fields]) bool,
) error {
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		record, ok, err := selector.Find(context.Background(), uuid)
		if err == nil && ok && predicate(record) {
			return nil
		}
		if err != nil && !verdandi.IsCode(err, verdandi.CodeUnavailable) {
			return err
		}
		time.Sleep(5 * time.Millisecond)
	}
	return fmt.Errorf("Go Selector did not observe %s", uuid)
}

func waitCatalog(
	entry *catalog.Entry,
	predicate func(catalog.Snapshot[verdandi.Fields]) bool,
) error {
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		snapshot, err := entry.Load[verdandi.Fields]()
		if err != nil {
			return err
		}
		if predicate(snapshot) {
			return nil
		}
		time.Sleep(5 * time.Millisecond)
	}
	return fmt.Errorf("Go Catalog Subscriber did not observe expected state")
}
