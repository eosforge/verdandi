package protocol

import (
	"bytes"
	"encoding/hex"
	"errors"
	"io"
	"testing"

	"google.golang.org/protobuf/proto"

	wire "github.com/eosforge/verdandi/supervisor/internal/generated"
)

func TestWireVectorsAndFragmentation(t *testing.T) {
	// Fixed independent wire vector: current Ping ID=15, length=2, request_id=1.
	want, _ := hex.DecodeString("000f000000020801")
	var buffer bytes.Buffer
	if err := Write(&buffer, &wire.Ping{RequestId: 1}, 32); err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(buffer.Bytes(), want) {
		t.Fatalf("wire mismatch: %x", buffer.Bytes())
	}
	message, err := Read(oneByteReader{bytes.NewReader(want)}, 32)
	if err != nil || !proto.Equal(message, &wire.Ping{RequestId: 1}) {
		t.Fatalf("fragmented read: %v, %v", message, err)
	}
}

func TestReadRejectsTruncationUnknownAndOversize(t *testing.T) {
	valid, _ := hex.DecodeString("000f000000020801")
	for length := range len(valid) {
		_, err := Read(bytes.NewReader(valid[:length]), 32)
		want := io.ErrUnexpectedEOF
		if length == 0 {
			want = io.EOF
		}
		if !errors.Is(err, want) {
			t.Fatalf("prefix %d: got %v, want %v", length, err, want)
		}
	}
	for _, raw := range []string{"ffff00000000", "000f00000021", "000f00000001ff"} {
		data, _ := hex.DecodeString(raw)
		if _, err := Read(bytes.NewReader(data), 32); !errors.Is(err, ErrFrame) {
			t.Fatalf("accepted invalid frame %s: %v", raw, err)
		}
	}
}

func TestWritesAreBoundedAndHandleShortWriters(t *testing.T) {
	for _, message := range []proto.Message{nil, (*wire.Ping)(nil), &wire.Ping{RequestId: 128}} {
		var buffer bytes.Buffer
		if err := Write(&buffer, message, 1); !errors.Is(err, ErrFrame) || buffer.Len() != 0 {
			t.Fatalf("invalid write changed stream: %v, %x", err, buffer.Bytes())
		}
	}
	if err := Write(zeroWriter{}, &wire.Ping{}, 32); !errors.Is(err, io.ErrShortWrite) {
		t.Fatalf("zero writer: %v", err)
	}
	var buffer bytes.Buffer
	if err := Write(oneByteWriter{&buffer}, &wire.Ping{RequestId: 1}, 32); err != nil || buffer.Len() != 8 {
		t.Fatalf("short writer: %v, %x", err, buffer.Bytes())
	}
}

type oneByteReader struct{ io.Reader }

func (r oneByteReader) Read(p []byte) (int, error) { return r.Reader.Read(p[:min(len(p), 1)]) }

type oneByteWriter struct{ io.Writer }

func (w oneByteWriter) Write(p []byte) (int, error) { return w.Writer.Write(p[:min(len(p), 1)]) }

type zeroWriter struct{}

func (zeroWriter) Write([]byte) (int, error) { return 0, nil }

func TestPhaseMismatchIsRejectedBeforeReadingOrDecodingPayload(t *testing.T) {
	// 完整头声明一份名单, 但不提供任何正文. 当前阶段必须直接拒绝, 不能读到 EOF 后才拒绝类型.
	header := []byte{0, 21, 0, 0, 4, 0}
	if _, err := Read(bytes.NewReader(header), 4096, wire.IDRegistrationRequest, wire.IDProtocolError); !errors.Is(err, ErrFrame) {
		t.Fatalf("phase guard read or decoded unexpected body: %v", err)
	}
}
