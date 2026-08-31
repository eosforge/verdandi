package verdandi

import (
	"fmt"
	"reflect"
	"strings"
	"testing"
	"time"
)

type redisHashFixture struct {
	Revision uint64 `redis:"@revision"`
	Name     string
	Ignored  string `redis:"-"`
	hidden   string
}

func TestRedisHashDescriptorUsesExactOrderedFields(t *testing.T) {
	descriptor := buildRedisHashDescriptor(reflect.TypeFor[redisHashFixture]())
	if descriptor.decodeErr != nil || descriptor.encodeErr != nil {
		t.Fatalf("unexpected descriptor errors: %v, %v", descriptor.decodeErr, descriptor.encodeErr)
	}
	want := []string{"@revision", "Name"}
	if !reflect.DeepEqual(descriptor.names, want) {
		t.Fatalf("got fields %v, want %v", descriptor.names, want)
	}

	client := &Client{}
	first := client.hashDescriptor(reflect.TypeFor[redisHashFixture]())
	second := client.hashDescriptor(reflect.TypeFor[redisHashFixture]())
	if first != second {
		t.Fatal("descriptor cache did not reuse the immutable plan")
	}
}

func TestRedisHashDescriptorRejectsAmbiguousShapes(t *testing.T) {
	type duplicate struct {
		Left  string `redis:"same"`
		Right string `redis:"same"`
	}
	type emptyTag struct {
		Value string `redis:""`
	}
	type unsupported struct {
		Value float64
	}
	type noFields struct {
		hidden string
	}

	tests := []struct {
		name      string
		valueType reflect.Type
	}{
		{name: "scalar", valueType: reflect.TypeFor[string]()},
		{name: "duplicate", valueType: reflect.TypeFor[duplicate]()},
		{name: "empty tag", valueType: reflect.TypeFor[emptyTag]()},
		{name: "unsupported field", valueType: reflect.TypeFor[unsupported]()},
		{name: "no fields", valueType: reflect.TypeFor[noFields]()},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			descriptor := buildRedisHashDescriptor(test.valueType)
			if descriptor.decodeErr == nil || descriptor.encodeErr == nil {
				t.Fatalf("descriptor unexpectedly accepted %v", test.valueType)
			}
		})
	}
}

func TestRedisHashDescriptorRejectsAggregateFieldNamesAboveCeiling(t *testing.T) {
	fieldCount := maxRedisHashBytes/maxRedisFieldNameBytes + 1
	fields := make([]reflect.StructField, fieldCount)
	for index := range fields {
		prefix := fmt.Sprintf("%04d", index)
		name := prefix + strings.Repeat("x", maxRedisFieldNameBytes-len(prefix))
		fields[index] = reflect.StructField{
			Name: fmt.Sprintf("Field%d", index),
			Type: reflect.TypeFor[string](),
			Tag:  reflect.StructTag(`redis:"` + name + `"`),
		}
	}
	descriptor := buildRedisHashDescriptor(reflect.StructOf(fields))
	if !IsCode(descriptor.decodeErr, CodeCapacity) || !IsCode(descriptor.encodeErr, CodeCapacity) {
		t.Fatalf("aggregate field names returned %v, %v; want capacity", descriptor.decodeErr, descriptor.encodeErr)
	}
}

func TestRootCommandsValidateBeforeRedisIO(t *testing.T) {
	client := &Client{
		config: runtimeConfig{
			timeout: time.Second,
		},
	}

	if _, _, err := client.Key().GetContext[string](nil, "key"); !IsCode(err, CodeInvalid) {
		t.Fatalf("nil context returned %v, want invalid", err)
	}
	if err := client.Key().Set("", "value"); !IsCode(err, CodeInvalid) {
		t.Fatalf("empty key returned %v, want invalid", err)
	}
	if err := client.Key().SetWithTTL("key", "value", time.Nanosecond); !IsCode(err, CodeInvalid) {
		t.Fatalf("sub-millisecond TTL returned %v, want invalid", err)
	}
	if _, err := client.Hash().Get[string]("key"); !IsCode(err, CodeContract) {
		t.Fatalf("scalar Hash type returned %v, want contract", err)
	}
	if err := client.Hash().Store("key", nil); !IsCode(err, CodeInvalid) {
		t.Fatalf("empty Fields returned %v, want invalid", err)
	}
	if _, err := client.Hash().Delete("key"); !IsCode(err, CodeInvalid) {
		t.Fatalf("empty field delete returned %v, want invalid", err)
	}

	var closed *Client
	if err := closed.Ping(); !IsCode(err, CodeClosed) {
		t.Fatalf("nil Client Ping returned %v, want closed", err)
	}
	if _, _, err := closed.Key().Load("key"); !IsCode(err, CodeClosed) {
		t.Fatalf("nil Client Key returned %v, want closed", err)
	}
	if _, err := closed.Hash().Load("key"); !IsCode(err, CodeClosed) {
		t.Fatalf("nil Client Hash returned %v, want closed", err)
	}
}
