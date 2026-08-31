package lifecycle

import (
	"context"
	"sync"
	"testing"
	"time"
)

func TestGateCancelsTracksAndRejectsAfterStart(t *testing.T) {
	var gate Gate
	ctx, cancel := context.WithCancel(context.Background())
	release, ok := gate.Track(cancel)
	if !ok {
		t.Fatal("initial Track rejected")
	}
	started := make(chan struct{})
	if !gate.Start(func() { close(started) }) {
		t.Fatal("first Start did not start")
	}
	select {
	case <-ctx.Done():
	case <-time.After(time.Second):
		t.Fatal("tracked context was not canceled")
	}
	if _, ok := gate.Track(nil); ok {
		t.Fatal("Track accepted after Start")
	}
	if gate.Start(nil) {
		t.Fatal("second Start reported first close")
	}
	release()
	release()
	gate.Wait()
	select {
	case <-started:
	default:
		t.Fatal("Start callback did not run")
	}
}

func TestGateWaitsForConcurrentRelease(t *testing.T) {
	var gate Gate
	const count = 64
	releases := make([]func(), count)
	for index := range releases {
		var ok bool
		releases[index], ok = gate.Track(nil)
		if !ok {
			t.Fatal("Track rejected before Start")
		}
	}
	gate.Start(nil)
	var workers sync.WaitGroup
	workers.Add(count)
	for _, release := range releases {
		go func() {
			defer workers.Done()
			release()
		}()
	}
	gate.Wait()
	workers.Wait()
}
