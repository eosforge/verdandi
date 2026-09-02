package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestGenerateViewsEditorsAndCheckMode(t *testing.T) {
	directory := t.TempDir()
	source := `package fixture

type Blob []byte
type Power int64

type Attr struct {
	Endpoint string
	Token Blob
}

type Data struct {
	Power, Queued Power
	Payload []byte
	Weights []Power
	Flags [4]byte
}
`
	if err := os.WriteFile(filepath.Join(directory, "model.go"), []byte(source), 0o600); err != nil {
		t.Fatal(err)
	}
	options := generatorOptions{
		directory: directory,
		attrName:  "Attr",
		dataName:  "Data",
		name:      "Proxy",
		output:    "reference_generated.go",
	}
	if err := generate(options); err != nil {
		t.Fatal(err)
	}
	generated, err := os.ReadFile(filepath.Join(directory, options.output))
	if err != nil {
		t.Fatal(err)
	}
	text := string(generated)
	for _, expected := range []string{
		"type ProxyAttrRef struct",
		"func (view ProxyAttrRef) Token() Blob",
		"verdandiregistration.ReferenceSlice[Blob, byte]",
		"return view.fieldToken.Clone()",
		"func (editor ProxyDataEditor) SetPower(value Power) error",
		"func (editor ProxyDataEditor) SetPayload(value []byte) error",
		"func (editor ProxyDataEditor) SetWeights(value []Power) error",
		"type ProxyReferenceSelector struct",
		"CloneData: func(value Data) Data",
	} {
		if !strings.Contains(text, expected) {
			t.Fatalf("generated source does not contain %q:\n%s", expected, text)
		}
	}
	for _, forbidden := range []string{
		"type ProxyAttrRef struct {\n\tvalue *Attr",
		"type ProxyDataRef struct {\n\tvalue *Data",
	} {
		if strings.Contains(text, forbidden) {
			t.Fatalf("generated source exposes a mutable model pointer %q:\n%s", forbidden, text)
		}
	}
	options.check = true
	if err := generate(options); err != nil {
		t.Fatalf("check current output: %v", err)
	}
	if err := os.WriteFile(filepath.Join(directory, options.output), append(generated, '\n'), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := generate(options); err == nil || !strings.Contains(err.Error(), "stale") {
		t.Fatalf("check stale output error = %v", err)
	}
}

func TestGenerateRejectsUnsafeAndAmbiguousFields(t *testing.T) {
	tests := []struct {
		name  string
		field string
		want  string
	}{
		{name: "map", field: "Labels map[string]string", want: "unsupported field type"},
		{name: "nested slice", field: "Weights [][]byte", want: "slices containing reference values"},
		{name: "pointer", field: "Target *int64", want: "unsupported field type"},
		{name: "private", field: "power int64", want: "must be exported"},
		{name: "embedded", field: "Embedded", want: "embedded fields"},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			directory := t.TempDir()
			source := "package fixture\n\ntype Embedded struct{}\ntype Attr struct { Region string }\ntype Data struct { " + test.field + " }\n"
			if err := os.WriteFile(filepath.Join(directory, "model.go"), []byte(source), 0o600); err != nil {
				t.Fatal(err)
			}
			err := generate(generatorOptions{
				directory: directory,
				attrName:  "Attr",
				dataName:  "Data",
				name:      "Proxy",
				output:    "generated.go",
			})
			if err == nil || !strings.Contains(err.Error(), test.want) {
				t.Fatalf("generate error = %v, want %q", err, test.want)
			}
		})
	}
}

func TestRunValidatesCommandLine(t *testing.T) {
	for _, arguments := range [][]string{
		{"-attr", "Attr", "-data", "Data", "-name", "private"},
		{"-attr", "Attr", "-data", "Data", "-name", "Proxy", "extra"},
	} {
		if err := run(arguments, &strings.Builder{}); err == nil {
			t.Fatalf("run(%q) succeeded", arguments)
		}
	}
}

func TestCommittedFixtureIsCurrent(t *testing.T) {
	err := generate(generatorOptions{
		directory: filepath.Join("..", "..", "internal", "refexample"),
		attrName:  "ProxyAttr",
		dataName:  "ProxyData",
		name:      "Proxy",
		output:    "reference_generated.go",
		check:     true,
	})
	if err != nil {
		t.Fatal(err)
	}
}
