package catalog

import (
	"encoding/hex"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
)

type catalogEventCorpus struct {
	Events []catalogEventVector `json:"events"`
}

type catalogEventVector struct {
	Name         string `json:"name"`
	PayloadHex   string `json:"payload_hex"`
	Operation    string `json:"operation"`
	Revision     uint64 `json:"revision"`
	BaseRevision uint64 `json:"base_revision"`
	Kind         string `json:"kind"`
	EncodedBytes int    `json:"encoded_bytes"`
	Path         struct {
		Part string `json:"part"`
		ID   string `json:"id"`
	} `json:"path"`
	Fields []struct {
		Name     string `json:"name"`
		ValueHex string `json:"value_hex"`
	} `json:"fields"`
}

func TestCatalogEventConformanceCorpus(t *testing.T) {
	t.Parallel()
	content, err := os.ReadFile(filepath.Join("..", "..", "..", "testkit", "conformance", "v1", "catalog_events.json"))
	if err != nil {
		t.Fatal(err)
	}
	var corpus catalogEventCorpus
	if err := json.Unmarshal(content, &corpus); err != nil {
		t.Fatal(err)
	}
	for _, vector := range corpus.Events {
		vector := vector
		t.Run(vector.Name, func(t *testing.T) {
			t.Parallel()
			payload, err := hex.DecodeString(vector.PayloadHex)
			if err != nil {
				t.Fatal(err)
			}
			path, err := NewPath(vector.Path.Part, vector.Path.ID)
			if err != nil {
				t.Fatal(err)
			}
			event, err := decodeEvent(string(payload), path, maximumEncodedBytes)
			if err != nil {
				t.Fatal(err)
			}
			kind := map[string]eventKind{"replace": eventReplace, "patch": eventPatch, "delete": eventDelete}[vector.Operation]
			if event.kind != kind || event.revision != vector.Revision || event.baseRevision != vector.BaseRevision ||
				event.encodedBytes != vector.EncodedBytes {
				t.Fatalf("event = %#v, vector = %#v", event, vector)
			}
			if vector.Kind != "" {
				valueKind, ok := parseKind(vector.Kind)
				if !ok || event.valueKind != valueKind {
					t.Fatalf("value kind = %v, want %q", event.valueKind, vector.Kind)
				}
			}
			if len(event.fields) != len(vector.Fields) {
				t.Fatalf("field count = %d, want %d", len(event.fields), len(vector.Fields))
			}
			for _, field := range vector.Fields {
				value, err := hex.DecodeString(field.ValueHex)
				if err != nil {
					t.Fatal(err)
				}
				if string(event.fields[field.Name]) != string(value) {
					t.Fatalf("field %q = %x, want %x", field.Name, event.fields[field.Name], value)
				}
			}
		})
	}
}
