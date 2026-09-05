package configuration

import (
	"encoding/json"
	"testing"

	verdandi "github.com/eosforge/verdandi/sdk/go"
)

func FuzzParseJSON(fuzzer *testing.F) {
	for _, seed := range [][]byte{
		[]byte(`{"version":"v1","redis":{"mode":"standalone","addresses":["127.0.0.1:6379"]}}`),
		[]byte(`{"version":"v1","redis":{"mode":"sentinel","addresses":["127.0.0.1:26379"],"master_name":"primary"}}`),
		[]byte(`{"version":"v1","version":"v1"}`),
		[]byte(`{"version":"v1"} {}`),
		[]byte{0xff},
		nil,
	} {
		fuzzer.Add(seed)
	}

	fuzzer.Fuzz(func(t *testing.T, source []byte) {
		config, err := ParseJSON(source)
		if len(source) > maximumJSONBytes {
			if !verdandi.IsCode(err, verdandi.CodeCapacity) {
				t.Fatalf("oversized source error = %v", err)
			}
			return
		}
		if err != nil {
			return
		}
		if config.Version != "v1" || config.check() != nil {
			t.Fatalf("accepted invalid configuration: %#v", config)
		}

		encoded, encodeErr := json.Marshal(config)
		if encodeErr != nil {
			t.Fatalf("accepted configuration cannot be encoded: %v", encodeErr)
		}
		if _, roundTripErr := ParseJSON(encoded); roundTripErr != nil {
			t.Fatalf("accepted configuration does not round-trip: %v", roundTripErr)
		}
	})
}
