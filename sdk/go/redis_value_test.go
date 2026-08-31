package verdandi

import (
	"bytes"
	"errors"
	"reflect"
	"testing"
)

type redisBinaryValue struct {
	text string
}

func (value *redisBinaryValue) MarshalBinary() ([]byte, error) {
	return append([]byte("binary:"), value.text...), nil
}

func (value *redisBinaryValue) UnmarshalBinary(source []byte) error {
	const prefix = "binary:"
	if !bytes.HasPrefix(source, []byte(prefix)) {
		return errors.New("missing binary prefix")
	}
	value.text = string(source[len(prefix):])
	return nil
}

func (value *redisBinaryValue) MarshalText() ([]byte, error) {
	return append([]byte("text:"), value.text...), nil
}

func (value *redisBinaryValue) UnmarshalText(source []byte) error {
	value.text = "text:" + string(source)
	return nil
}

type redisTextValue string

func (value *redisTextValue) MarshalText() ([]byte, error) {
	return []byte("text:" + string(*value)), nil
}

func (value *redisTextValue) UnmarshalText(source []byte) error {
	const prefix = "text:"
	if !bytes.HasPrefix(source, []byte(prefix)) {
		return errors.New("missing text prefix")
	}
	*value = redisTextValue(source[len(prefix):])
	return nil
}

func TestRedisScalarBuiltinsUseCanonicalEncoding(t *testing.T) {
	tests := []struct {
		name  string
		value any
		want  string
	}{
		{name: "false", value: false, want: "0"},
		{name: "true", value: true, want: "1"},
		{name: "signed", value: int64(-9223372036854775808), want: "-9223372036854775808"},
		{name: "unsigned", value: uint64(18446744073709551615), want: "18446744073709551615"},
		{name: "string", value: "value", want: "value"},
		{name: "bytes", value: []byte("bytes"), want: "bytes"},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			var encoded []byte
			var err error
			switch value := test.value.(type) {
			case bool:
				encoded, err = encodeRedisValue(value, "value")
			case int64:
				encoded, err = encodeRedisValue(value, "value")
			case uint64:
				encoded, err = encodeRedisValue(value, "value")
			case string:
				encoded, err = encodeRedisValue(value, "value")
			case []byte:
				encoded, err = encodeRedisValue(value, "value")
			default:
				t.Fatalf("missing fixture branch for %T", value)
			}
			if err != nil {
				t.Fatal(err)
			}
			if string(encoded) != test.want {
				t.Fatalf("got %q, want %q", encoded, test.want)
			}
		})
	}

	if decoded, err := decodeRedisValue[int8]([]byte("-128"), "value"); err != nil || decoded != -128 {
		t.Fatalf("unexpected int8 decode: %d, %v", decoded, err)
	}
	if decoded, err := decodeRedisValue[uint8]([]byte("255"), "value"); err != nil || decoded != 255 {
		t.Fatalf("unexpected uint8 decode: %d, %v", decoded, err)
	}
	if decoded, err := decodeRedisValue[bool]([]byte("1"), "value"); err != nil || !decoded {
		t.Fatalf("unexpected bool decode: %t, %v", decoded, err)
	}
}

func TestRedisScalarRejectsNoncanonicalAndUnsupportedTypes(t *testing.T) {
	for _, source := range []string{"", "+1", "01", "-0", " 1"} {
		if _, err := decodeRedisValue[int64]([]byte(source), "value"); !IsCode(err, CodeCorrupt) {
			t.Fatalf("%q returned %v, want corrupt", source, err)
		}
	}
	for _, source := range []string{"false", "true", "2", ""} {
		if _, err := decodeRedisValue[bool]([]byte(source), "value"); !IsCode(err, CodeCorrupt) {
			t.Fatalf("%q returned %v, want corrupt", source, err)
		}
	}
	if _, err := decodeRedisValue[int]([]byte("1"), "value"); !IsCode(err, CodeContract) {
		t.Fatalf("int returned %v, want contract", err)
	}
	if _, err := encodeRedisValue(float64(1), "value"); !IsCode(err, CodeContract) {
		t.Fatalf("float64 returned %v, want contract", err)
	}
	if _, err := encodeRedisValue(map[string]string{"a": "b"}, "value"); !IsCode(err, CodeContract) {
		t.Fatalf("map returned %v, want contract", err)
	}
	value := uint64(1)
	if _, err := encodeRedisValue(&value, "value"); !IsCode(err, CodeContract) {
		t.Fatalf("pointer returned %v, want contract", err)
	}
}

func TestRedisScalarUsesBinaryBeforeTextAndDetachesBytes(t *testing.T) {
	value := redisBinaryValue{text: "payload"}
	encoded, err := encodeRedisValue(value, "value")
	if err != nil {
		t.Fatal(err)
	}
	if string(encoded) != "binary:payload" {
		t.Fatalf("unexpected custom encoding %q", encoded)
	}
	decoded, err := decodeRedisValue[redisBinaryValue](encoded, "value")
	if err != nil || decoded.text != "payload" {
		t.Fatalf("unexpected custom decode: %#v, %v", decoded, err)
	}

	text := redisTextValue("payload")
	encoded, err = encodeRedisValue(text, "value")
	if err != nil || string(encoded) != "text:payload" {
		t.Fatalf("unexpected text encoding %q, %v", encoded, err)
	}
	decodedText, err := decodeRedisValue[redisTextValue](encoded, "value")
	if err != nil || decodedText != text {
		t.Fatalf("unexpected text decode %q, %v", decodedText, err)
	}

	source := []byte("owned")
	decodedBytes, err := decodeRedisValue[[]byte](source, "value")
	if err != nil {
		t.Fatal(err)
	}
	source[0] = 'X'
	if string(decodedBytes) != "owned" {
		t.Fatalf("decoded bytes retained caller storage: %q", decodedBytes)
	}
}

func TestRedisScalarTypeSupportIsStatic(t *testing.T) {
	type namedUint64 uint64
	if !supportsRedisEncode(reflect.TypeFor[namedUint64]()) || !supportsRedisDecode(reflect.TypeFor[namedUint64]()) {
		t.Fatal("named fixed-width integer should be supported")
	}
	if supportsRedisEncode(reflect.TypeFor[uint]()) || supportsRedisDecode(reflect.TypeFor[uint]()) {
		t.Fatal("machine-width uint should be rejected")
	}
	if supportsRedisEncode(reflect.TypeFor[any]()) || supportsRedisDecode(reflect.TypeFor[any]()) {
		t.Fatal("interface types should be rejected")
	}
}

func TestRedisScalarCapacityIsEnforcedBeforeCommands(t *testing.T) {
	oversized := make([]byte, maxRedisValueBytes+1)
	if _, err := encodeRedisValue(oversized, "value"); !IsCode(err, CodeCapacity) {
		t.Fatalf("oversized encode returned %v, want capacity", err)
	}
	if _, err := decodeRedisValue[[]byte](oversized, "value"); !IsCode(err, CodeCapacity) {
		t.Fatalf("oversized decode returned %v, want capacity", err)
	}
}
