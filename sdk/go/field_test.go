package verdandi

import "testing"

func TestFieldsEncodeReturnsDetachedFields(t *testing.T) {
	source := Fields{"value": []byte("one")}
	encoded, err := source.Encode()
	if err != nil {
		t.Fatalf("Fields.Encode error = %v", err)
	}

	encoded["value"][0] = 't'
	encoded["added"] = []byte("field")
	if string(source["value"]) != "one" {
		t.Fatalf("encoded value aliased source: %q", source["value"])
	}
	if _, exists := source["added"]; exists {
		t.Fatal("encoded map aliased source")
	}
}

func TestFieldsDecodeRejectsNilReceiver(t *testing.T) {
	var destination *Fields
	if err := destination.Decode(Fields{}); !IsCode(err, CodeInvalid) {
		t.Fatalf("Fields.Decode error = %v, want invalid", err)
	}
}
