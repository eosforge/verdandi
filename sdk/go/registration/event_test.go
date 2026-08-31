package registration

import (
	"bytes"
	"testing"

	"github.com/vmihailenco/msgpack/v5"
	"github.com/vmihailenco/msgpack/v5/msgpcode"
)

func TestDecodeRegistrationEvent(t *testing.T) {
	t.Parallel()
	values := []any{
		"&protocol", "v1",
		"&kind", "register",
		"@uuid", "0123456789abcdef0123456789abcdef",
		"@revision", int64(7),
		"@timestamp", int64(1787371200123),
		"@ttl", int64(15000),
		"@version", int64(3),
		".region", []byte("cn-east"),
		"payload", []byte{0, 255, 10},
	}
	payload, err := msgpack.Marshal(values)
	if err != nil {
		t.Fatal(err)
	}
	event, err := decodeRegistrationEvent(payload, defaultZoneConfig())
	if err != nil {
		t.Fatalf("decodeRegistrationEvent() error = %v", err)
	}
	if event.kind != "register" || event.revision != 7 || event.version != 3 {
		t.Fatalf("unexpected event: %#v", event)
	}
	if !bytes.Equal(event.data["payload"], []byte{0, 255, 10}) {
		t.Fatalf("binary payload = %v", event.data["payload"])
	}
}

func TestDecodeRegistrationEventRejectsMalformedInput(t *testing.T) {
	t.Parallel()
	tests := []struct {
		name   string
		values []any
		code   Code
	}{
		{
			name:   "odd pairs",
			values: []any{"&protocol", "v1", "&kind"},
			code:   CodeCorrupt,
		},
		{
			name: "duplicate",
			values: []any{
				"&protocol", "v1", "&protocol", "v1", "&kind", "unregister",
				"@uuid", "0123456789abcdef0123456789abcdef",
			},
			code: CodeContract,
		},
		{
			name: "renew with data",
			values: []any{
				"&protocol", "v1", "&kind", "renew",
				"@uuid", "0123456789abcdef0123456789abcdef",
				"@revision", int64(1), "@timestamp", int64(2), "load", []byte("1"),
			},
			code: CodeContract,
		},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			payload, err := msgpack.Marshal(test.values)
			if err != nil {
				t.Fatal(err)
			}
			_, err = decodeRegistrationEvent(payload, defaultZoneConfig())
			if !IsCode(err, test.code) {
				t.Fatalf("decodeRegistrationEvent() error = %v, want code %q", err, test.code)
			}
		})
	}
}

func TestDecodeRegistrationEventRejectsExpansionMarkers(t *testing.T) {
	t.Parallel()
	nestedMap := []byte("\x96\xa9&protocol\xa2v1\xa500000\xdf0000000000\xa5@uuid\xd9 00000000000000'000otoc0000000000")
	if _, err := decodeRegistrationEvent(nestedMap, defaultZoneConfig()); !IsCode(err, CodeCorrupt) {
		t.Fatalf("nested map error = %v, want corrupt", err)
	}
	excessiveArray := []byte{0xdd, 0x00, 0x00, 0x02, 0x10}
	if _, err := decodeRegistrationEvent(excessiveArray, defaultZoneConfig()); !IsCode(err, CodeCapacity) {
		t.Fatalf("excessive array error = %v, want capacity", err)
	}

	valid, err := msgpack.Marshal([]any{
		"&protocol", "v1", "&kind", "unregister",
		"@uuid", "0123456789abcdef0123456789abcdef",
	})
	if err != nil {
		t.Fatal(err)
	}
	valid = append(valid, 0)
	if _, err := decodeRegistrationEvent(valid, defaultZoneConfig()); !IsCode(err, CodeCorrupt) {
		t.Fatalf("trailing byte error = %v, want corrupt", err)
	}
}

func TestDecodeEventBytesRejectsImpossibleLengthBeforeReadingBody(t *testing.T) {
	t.Parallel()
	decoder := eventDecoder{input: []byte{0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0}}
	if _, err := decoder.bytes(); err == nil {
		t.Fatal("declared multi-gigabyte value was accepted")
	}
	if decoder.offset != 5 {
		t.Fatalf("decoder consumed malformed body: offset = %d, want header length 5", decoder.offset)
	}
}

func TestDecodeRegistrationEventIgnoresUnknownScalarControls(t *testing.T) {
	t.Parallel()
	for name, value := range map[string]any{
		"nil":     nil,
		"boolean": true,
		"integer": int64(-1),
		"float":   1.5,
		"string":  "future",
		"binary":  []byte{0, 255},
	} {
		t.Run(name, func(t *testing.T) {
			t.Parallel()
			payload, err := msgpack.Marshal([]any{
				"&protocol", "v1", "&kind", "unregister",
				"@uuid", "0123456789abcdef0123456789abcdef",
				"&future", value,
			})
			if err != nil {
				t.Fatal(err)
			}
			if _, err := decodeRegistrationEvent(payload, defaultZoneConfig()); err != nil {
				t.Fatalf("unknown scalar control error = %v", err)
			}
		})
	}
}

func TestEventDecoderAcceptsEverySupportedByteAndIntegerWidth(t *testing.T) {
	t.Parallel()
	arrays := []struct {
		name    string
		encoded []byte
		want    int
	}{
		{name: "fixed", encoded: []byte{0x92}, want: 2},
		{name: "array16", encoded: []byte{msgpcode.Array16, 0, 2}, want: 2},
		{name: "array32", encoded: []byte{msgpcode.Array32, 0, 0, 0, 2}, want: 2},
		{name: "array32 bounded", encoded: []byte{msgpcode.Array32, 0xff, 0xff, 0xff, 0xff}, want: maxRegistrationEventBytes + 1},
	}
	for _, test := range arrays {
		t.Run("array/"+test.name, func(t *testing.T) {
			decoder := eventDecoder{input: test.encoded}
			value, err := decoder.arrayLen()
			if err != nil || value != test.want || !decoder.done() {
				t.Fatalf("arrayLen() = %d, %v, done=%v", value, err, decoder.done())
			}
		})
	}

	byteValues := []struct {
		name    string
		encoded []byte
	}{
		{name: "fixed", encoded: []byte{0xa1, 'x'}},
		{name: "str8", encoded: []byte{msgpcode.Str8, 1, 'x'}},
		{name: "str16", encoded: []byte{msgpcode.Str16, 0, 1, 'x'}},
		{name: "str32", encoded: []byte{msgpcode.Str32, 0, 0, 0, 1, 'x'}},
		{name: "bin8", encoded: []byte{msgpcode.Bin8, 1, 'x'}},
		{name: "bin16", encoded: []byte{msgpcode.Bin16, 0, 1, 'x'}},
		{name: "bin32", encoded: []byte{msgpcode.Bin32, 0, 0, 0, 1, 'x'}},
	}
	for _, test := range byteValues {
		t.Run("bytes/"+test.name, func(t *testing.T) {
			decoder := eventDecoder{input: test.encoded}
			value, err := decoder.bytes()
			if err != nil || !bytes.Equal(value, []byte{'x'}) || !decoder.done() {
				t.Fatalf("bytes() = %v, %v, done=%v", value, err, decoder.done())
			}
		})
	}

	integers := []struct {
		name    string
		encoded []byte
	}{
		{name: "fixed", encoded: []byte{42}},
		{name: "uint8", encoded: []byte{msgpcode.Uint8, 42}},
		{name: "uint16", encoded: []byte{msgpcode.Uint16, 0, 42}},
		{name: "uint32", encoded: []byte{msgpcode.Uint32, 0, 0, 0, 42}},
		{name: "uint64", encoded: []byte{msgpcode.Uint64, 0, 0, 0, 0, 0, 0, 0, 42}},
		{name: "int8", encoded: []byte{msgpcode.Int8, 42}},
		{name: "int16", encoded: []byte{msgpcode.Int16, 0, 42}},
		{name: "int32", encoded: []byte{msgpcode.Int32, 0, 0, 0, 42}},
		{name: "int64", encoded: []byte{msgpcode.Int64, 0, 0, 0, 0, 0, 0, 0, 42}},
	}
	for _, test := range integers {
		t.Run("integer/"+test.name, func(t *testing.T) {
			decoder := eventDecoder{input: test.encoded}
			value, err := decoder.uint()
			if err != nil || value != 42 || !decoder.done() {
				t.Fatalf("uint() = %d, %v, done=%v", value, err, decoder.done())
			}
		})
	}
}

func TestEventDecoderRejectsNonScalarAndInvalidIntegers(t *testing.T) {
	t.Parallel()
	for name, encoded := range map[string][]byte{
		"array":     {0x90},
		"map":       {0x80},
		"extension": {msgpcode.FixExt1, 0, 0},
		"reserved":  {0xc1},
	} {
		t.Run(name, func(t *testing.T) {
			decoder := eventDecoder{input: encoded}
			if err := decoder.skipScalar(); err == nil {
				t.Fatal("non-scalar value was accepted")
			}
		})
	}
	for name, encoded := range map[string][]byte{
		"zero":      {0},
		"negative":  {0xff},
		"truncated": {msgpcode.Uint64, 0},
	} {
		t.Run("integer/"+name, func(t *testing.T) {
			decoder := eventDecoder{input: encoded}
			if _, err := decoder.uint(); err == nil {
				t.Fatal("invalid integer was accepted")
			}
		})
	}
}

func FuzzDecodeRegistrationEvent(f *testing.F) {
	seed, _ := msgpack.Marshal([]any{
		"&protocol", "v1", "&kind", "unregister",
		"@uuid", "0123456789abcdef0123456789abcdef",
	})
	f.Add(seed)
	f.Add([]byte{0x91, 0xa1, 'x'})
	f.Fuzz(func(t *testing.T, payload []byte) {
		_, _ = decodeRegistrationEvent(payload, defaultZoneConfig())
	})
}
