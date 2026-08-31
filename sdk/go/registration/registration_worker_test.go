package registration

import (
	"bytes"
	"context"
	"testing"
	"time"
)

func TestRegistrationRawUpdateDetachesCallerState(t *testing.T) {
	t.Parallel()
	version := uint64(2)
	data := Fields{"load": {1, 2, 3}}

	detached := detachUpdate(registrationUpdateFields{Version: &version, Data: data})
	version = 3
	data["load"][0] = 9
	data["added"] = []byte{4}

	if detached.Version == nil || *detached.Version != 2 {
		t.Fatalf("detached Version = %v, want 2", detached.Version)
	}
	if !bytes.Equal(detached.Data["load"], []byte{1, 2, 3}) {
		t.Fatalf("detached Data = %v, want [1 2 3]", detached.Data["load"])
	}
	if _, exists := detached.Data["added"]; exists {
		t.Fatal("detached Data retained caller map alias")
	}
}

func TestRegistrationFieldsMailboxMergesIntoOneBatch(t *testing.T) {
	t.Parallel()
	client := newTestRuntime(runtimeConfig{}, protocolZoneConfig())
	registration := &registrationCore{
		client:    client,
		dataShape: Fields{"power": nil},
		wake:      make(chan struct{}, 1),
		slots:     make(chan struct{}, 8),
		closing:   make(chan struct{}),
		done:      make(chan struct{}),
	}
	results := make(chan error, 3)
	go func() {
		results <- registration.updateOwned(context.Background(), registrationUpdateFields{
			Data: Fields{"power": []byte("2")},
		})
	}()
	waitPendingUpdates(t, registration, 1)

	version := uint64(3)
	go func() {
		results <- registration.updateOwned(context.Background(), registrationUpdateFields{
			Version: &version,
			Data:    Fields{"power": []byte("4")},
		})
	}()
	waitPendingUpdates(t, registration, 2)
	go func() {
		results <- registration.Renew(context.Background())
	}()
	waitPendingRenews(t, registration, 1)

	batch, exists := registration.takeBatch()
	if !exists {
		t.Fatal("mailbox is empty, want one merged batch")
	}
	if !batch.hasVersion || batch.version != 3 {
		t.Fatalf("batch version = (%d, %t), want (3, true)", batch.version, batch.hasVersion)
	}
	if !bytes.Equal(batch.data["power"], []byte("4")) {
		t.Fatalf("batch power = %q, want 4", batch.data["power"])
	}
	if len(batch.updates) != 2 {
		t.Fatalf("batch update waiters = %d, want 2", len(batch.updates))
	}
	if len(batch.renews) != 1 {
		t.Fatalf("batch renew waiters = %d, want 1", len(batch.renews))
	}
	for _, result := range batch.updates {
		result <- nil
	}
	for _, result := range batch.renews {
		result <- nil
	}
	for range len(batch.updates) + len(batch.renews) {
		if err := <-results; err != nil {
			t.Fatalf("merged operation = %v, want nil", err)
		}
	}
}

func TestRegistrationFieldsMailboxAdmissionCanBeCancelled(t *testing.T) {
	t.Parallel()
	registration := &registrationCore{
		slots:   make(chan struct{}, 1),
		closing: make(chan struct{}),
		client:  &clientRuntime{done: make(chan struct{}), transportDone: make(chan struct{})},
	}
	if err := registration.acquireSlot(context.Background()); err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if err := registration.acquireSlot(ctx); !isCode(err, codeClosed) {
		t.Fatalf("acquireSlot() error = %v, want closed cancellation", err)
	}
	registration.releaseSlot()
}

func waitPendingUpdates(t *testing.T, registration *registrationCore, count int) {
	t.Helper()
	deadline := time.Now().Add(time.Second)
	for time.Now().Before(deadline) {
		registration.bufferMu.Lock()
		pending := len(registration.pendingUpdates)
		registration.bufferMu.Unlock()
		if pending == count {
			return
		}
		time.Sleep(time.Millisecond)
	}
	t.Fatalf("pending updates did not reach %d", count)
}

func waitPendingRenews(t *testing.T, registration *registrationCore, count int) {
	t.Helper()
	deadline := time.Now().Add(time.Second)
	for time.Now().Before(deadline) {
		registration.bufferMu.Lock()
		pending := len(registration.pendingRenews)
		registration.bufferMu.Unlock()
		if pending == count {
			return
		}
		time.Sleep(time.Millisecond)
	}
	t.Fatalf("pending renews did not reach %d", count)
}
