package main

import (
	"bufio"
	"context"
	"crypto/tls"
	"crypto/x509"
	"fmt"
	"os"
	"strings"
	"time"

	verdandi "github.com/eosforge/verdandi/sdk/go"
	discovery "github.com/eosforge/verdandi/sdk/go/registration"
)

func main() {
	if err := run(); err != nil {
		_, _ = fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

func run() error {
	if len(os.Args) != 2 {
		return fmt.Errorf("usage: go-peer <zone>")
	}
	addresses := strings.Split(os.Getenv("VERDANDI_SENTINEL_ADDRS"), ",")
	if len(addresses) == 1 && addresses[0] == "" {
		return fmt.Errorf("VERDANDI_SENTINEL_ADDRS is required")
	}
	tlsConfig, err := sentinelTLS()
	if err != nil {
		return err
	}
	client, err := verdandi.Open(context.Background(), verdandi.Config{
		Sentinel: &verdandi.Sentinel{
			Addresses:        addresses,
			MasterName:       os.Getenv("VERDANDI_SENTINEL_MASTER"),
			Username:         os.Getenv("VERDANDI_REDIS_USERNAME"),
			Password:         os.Getenv("VERDANDI_REDIS_PASSWORD"),
			SentinelUsername: os.Getenv("VERDANDI_SENTINEL_USERNAME"),
			SentinelPassword: os.Getenv("VERDANDI_SENTINEL_PASSWORD"),
			TLS:              tlsConfig,
		},
		Timeout: 3 * time.Second,
	})
	if err != nil {
		return err
	}
	registrationClient, err := discovery.Open(context.Background(), client, discovery.Config{
		Zone:                    os.Args[1],
		SelectorPageSize:        64,
		SelectorEventBuffer:     1024,
		SelectorEventBytes:      16 * 1024 * 1024,
		SelectorPublishInterval: new(time.Millisecond),
		SelectorSyncTimeout:     20 * time.Second,
		ClockRefresh:            time.Second,
	})
	if err != nil {
		return err
	}
	selector, err := registrationClient.Selector[verdandi.Fields, verdandi.Fields](
		context.Background(),
		discovery.SelectorOptions{Type: "fault"},
	)
	if err != nil {
		return err
	}
	registration, err := registrationClient.Registration[verdandi.Fields, verdandi.Fields](discovery.RegistrationOptions{
		Type:          "fault",
		TTL:           5 * time.Minute,
		RenewInterval: time.Minute,
		Version:       1,
	})
	if err != nil {
		return err
	}
	if err := registration.Register(
		context.Background(),
		verdandi.Fields{"language": []byte("go")},
		verdandi.Fields{"value": []byte("initial")},
	); err != nil {
		return err
	}
	go reportErrors("registration-client", registrationClient.Errors())
	go reportErrors("registration", registration.Errors())
	go reportErrors("selector", selector.Errors())
	output("READY %s", registration.UUID())
	desired := "initial"

	scanner := bufio.NewScanner(os.Stdin)
	for scanner.Scan() {
		parts := strings.Fields(scanner.Text())
		if len(parts) == 0 {
			continue
		}
		switch parts[0] {
		case "UPDATE":
			if len(parts) != 2 {
				return fmt.Errorf("UPDATE requires one value")
			}
			operation, cancel := context.WithTimeout(context.Background(), 30*time.Second)
			err := registration.Update(operation, verdandi.Fields{"value": []byte(parts[1])})
			cancel()
			outcome := "ok"
			if err != nil {
				if !verdandi.IsCode(err, verdandi.CodeAmbiguous) {
					return err
				}
				outcome = "ambiguous"
			}
			if err := waitRecord(selector, registration.UUID(), parts[1], registration.Revision()); err != nil {
				return err
			}
			desired = parts[1]
			output("UPDATED %s %d %d %s", registration.UUID(), registration.Revision(), registration.Timestamp(), outcome)
		case "RENEW":
			if err := renewUntilConfirmed(registration, 30*time.Second); err != nil {
				return err
			}
			if err := waitRecord(selector, registration.UUID(), desired, registration.Revision()); err != nil {
				return err
			}
			output("RENEWED %s %d %d", registration.UUID(), registration.Revision(), registration.Timestamp())
		case "CHECK":
			if len(parts) != 3 {
				return fmt.Errorf("CHECK requires UUID and value")
			}
			record, generation, err := waitValue(selector, parts[1], parts[2])
			if err != nil {
				return err
			}
			output("CHECKED %s %d %d", parts[1], record.Meta.Revision, generation)
		case "WAIT_UNSYNC":
			deadline := time.Now().Add(15 * time.Second)
			last, _ := selector.Snapshot(context.Background())
			for time.Now().Before(deadline) {
				_, snapshotErr := selector.Snapshot(context.Background())
				if verdandi.IsCode(snapshotErr, verdandi.CodeUnavailable) {
					break
				}
				if snapshotErr != nil {
					return snapshotErr
				}
				time.Sleep(10 * time.Millisecond)
			}
			if _, snapshotErr := selector.Snapshot(context.Background()); !verdandi.IsCode(snapshotErr, verdandi.CodeUnavailable) {
				return fmt.Errorf("Selector remained synchronized")
			}
			output("UNSYNCHRONIZED %d", last.Generation)
		case "STOP":
			closeContext, cancel := context.WithTimeout(context.Background(), 15*time.Second)
			defer cancel()
			if err := registration.Close(closeContext); err != nil {
				return err
			}
			if err := selector.Close(closeContext); err != nil {
				return err
			}
			if err := registrationClient.Close(closeContext); err != nil {
				return err
			}
			if err := client.Close(); err != nil {
				return err
			}
			output("STOPPED")
			return nil
		default:
			return fmt.Errorf("unknown command %q", parts[0])
		}
	}
	return scanner.Err()
}

func sentinelTLS() (*tls.Config, error) {
	path := os.Getenv("VERDANDI_TLS_CA_FILE")
	if path == "" {
		return nil, nil
	}
	serverName := os.Getenv("VERDANDI_TLS_SERVER_NAME")
	if serverName == "" {
		return nil, fmt.Errorf("VERDANDI_TLS_SERVER_NAME is required with VERDANDI_TLS_CA_FILE")
	}
	certificate, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	roots := x509.NewCertPool()
	if !roots.AppendCertsFromPEM(certificate) {
		return nil, fmt.Errorf("VERDANDI_TLS_CA_FILE contains no certificate")
	}
	return &tls.Config{
		MinVersion: tls.VersionTLS12,
		RootCAs:    roots,
		ServerName: serverName,
	}, nil
}

func renewUntilConfirmed(registration *discovery.Registration[verdandi.Fields, verdandi.Fields], timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	for {
		operation, cancel := context.WithTimeout(context.Background(), 4*time.Second)
		err := registration.Renew(operation)
		cancel()
		if err == nil {
			return nil
		}
		if !verdandi.IsCode(err, verdandi.CodeAmbiguous) &&
			!verdandi.IsCode(err, verdandi.CodeUnavailable) &&
			!verdandi.IsCode(err, verdandi.CodeDeadline) {
			return err
		}
		if time.Now().After(deadline) {
			return err
		}
		time.Sleep(100 * time.Millisecond)
	}
}

func waitRecord(selector *discovery.Selector[verdandi.Fields, verdandi.Fields], uuid string, value string, revision uint64) error {
	_, _, err := waitFor(selector, uuid, func(record discovery.Candidate[verdandi.Fields, verdandi.Fields]) bool {
		return record.Meta.Revision == revision && string((*record.Data)["value"]) == value
	})
	return err
}

func waitValue(
	selector *discovery.Selector[verdandi.Fields, verdandi.Fields],
	uuid string,
	value string,
) (discovery.Candidate[verdandi.Fields, verdandi.Fields], uint64, error) {
	return waitFor(selector, uuid, func(record discovery.Candidate[verdandi.Fields, verdandi.Fields]) bool {
		return string((*record.Data)["value"]) == value
	})
}

func waitFor(
	selector *discovery.Selector[verdandi.Fields, verdandi.Fields],
	uuid string,
	predicate func(discovery.Candidate[verdandi.Fields, verdandi.Fields]) bool,
) (discovery.Candidate[verdandi.Fields, verdandi.Fields], uint64, error) {
	deadline := time.Now().Add(30 * time.Second)
	for time.Now().Before(deadline) {
		snapshot, snapshotErr := selector.Snapshot(context.Background())
		if snapshotErr == nil {
			if record, ok, findErr := selector.Find(context.Background(), uuid); findErr == nil && ok && predicate(record) {
				return record, snapshot.Generation, nil
			}
		} else if !verdandi.IsCode(snapshotErr, verdandi.CodeUnavailable) {
			return discovery.Candidate[verdandi.Fields, verdandi.Fields]{}, 0, snapshotErr
		}
		time.Sleep(10 * time.Millisecond)
	}
	return discovery.Candidate[verdandi.Fields, verdandi.Fields]{}, 0, fmt.Errorf("Selector did not converge for %s", uuid)
}

func reportErrors(owner string, errors <-chan error) {
	for err := range errors {
		_, _ = fmt.Fprintf(os.Stderr, "%s: %v\n", owner, err)
	}
}

func output(format string, values ...any) {
	fmt.Printf(format+"\n", values...)
}
