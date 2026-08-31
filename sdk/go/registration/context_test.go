package registration

import (
	"context"
	"errors"
	"testing"
	"time"
)

func TestMergeContexts(t *testing.T) {
	t.Parallel()
	parent, parentCancel := context.WithCancel(context.Background())
	owner, ownerCancel := context.WithCancel(context.Background())
	merged, cancel := mergeContexts(parent, owner)
	ownerCancel()
	select {
	case <-merged.Done():
	case <-time.After(time.Second):
		t.Fatal("owner cancellation did not reach merged context")
	}
	cancel()
	parentCancel()
	if !errors.Is(merged.Err(), context.Canceled) {
		t.Fatalf("merged.Err() = %v", merged.Err())
	}
}

func TestAdmittedWriteReturnsWriterOutcomeAfterCallerCancellation(t *testing.T) {
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
	requestContext, cancel := context.WithCancel(context.Background())
	result := make(chan error, 1)
	go func() {
		result <- registration.updateOwned(requestContext, registrationUpdateFields{
			Data: Fields{"power": []byte("2")},
		})
	}()
	waitPendingUpdates(t, registration, 1)
	batch, exists := registration.takeBatch()
	if !exists || len(batch.updates) != 1 {
		t.Fatalf("takeBatch() = (%+v, %t), want one update", batch, exists)
	}
	cancel()
	time.Sleep(10 * time.Millisecond)
	batch.updates[0] <- protocolError(codeAmbiguous, "", 0)
	err := <-result
	if !isCode(err, codeAmbiguous) {
		t.Fatalf("updateOwned() error = %v, want ambiguous writer outcome", err)
	}
}
