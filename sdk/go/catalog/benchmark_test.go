package catalog

import (
	"fmt"
	"strconv"
	"testing"

	verdandi "github.com/LaconisIves/verdandi/sdk/go"
	"github.com/vmihailenco/msgpack/v5"
)

func BenchmarkValidateArray(b *testing.B) {
	const count = 512
	fields := make(verdandi.Fields, count)
	for index := range count {
		fields[strconv.Itoa(index)] = []byte("value")
	}
	b.ReportAllocs()
	for b.Loop() {
		if _, _, err := validateValue(Array, fields, maximumEncodedBytes); err != nil {
			b.Fatal(err)
		}
	}
}

func BenchmarkDecodeReplaceEvent(b *testing.B) {
	const count = 512
	path, err := NewPath("routing", "benchmark")
	if err != nil {
		b.Fatal(err)
	}
	fields := make([]any, 0, count*2)
	encodedBytes := 0
	for index := range count {
		name := fmt.Sprintf("field-%03d", index)
		value := []byte("0123456789abcdef0123456789abcdef")
		fields = append(fields, name, value)
		encodedBytes += len(name) + len(value)
	}
	encoded, err := msgpack.Marshal([]any{
		"v1", "replace", path.member(), "12", "map", strconv.Itoa(encodedBytes), fields,
	})
	if err != nil {
		b.Fatal(err)
	}
	payload := string(encoded)
	b.ReportAllocs()
	for b.Loop() {
		event, decodeErr := decodeEvent(payload, path, maximumEncodedBytes)
		if decodeErr != nil {
			b.Fatal(decodeErr)
		}
		if len(event.fields) != count {
			b.Fatalf("decoded %d fields", len(event.fields))
		}
	}
}
