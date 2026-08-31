//go:build integration && load && soak

package catalog

import "testing"

func TestCatalogSoakSubscriberClient(t *testing.T) {
	memoryClient := &Client{}
	persistentClient := &Client{}
	tests := []struct {
		name                  string
		subscribers           int
		persistentSubscribers int
	}{
		{name: "none", subscribers: 4, persistentSubscribers: 0},
		{name: "default one", subscribers: 4, persistentSubscribers: 1},
		{name: "explicit subset", subscribers: 8, persistentSubscribers: 2},
		{name: "all", subscribers: 2, persistentSubscribers: 2},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			for index := range test.subscribers {
				actual := catalogSoakSubscriberClient(index, test.persistentSubscribers, memoryClient, persistentClient)
				want := memoryClient
				if index < test.persistentSubscribers {
					want = persistentClient
				}
				if actual != want {
					t.Fatalf("subscriber[%d] client = %p, want %p", index, actual, want)
				}
			}
		})
	}
}
