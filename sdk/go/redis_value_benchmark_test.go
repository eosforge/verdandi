package verdandi

import "testing"

func BenchmarkDecodeRedisInt64(b *testing.B) {
	source := []byte("-9223372036854775808")
	b.ReportAllocs()
	for b.Loop() {
		value, err := decodeRedisValue[int64](source, "value")
		if err != nil || value != -9223372036854775808 {
			b.Fatalf("decode = %d, %v", value, err)
		}
	}
}

func BenchmarkDecodeRedisUint64(b *testing.B) {
	source := []byte("18446744073709551615")
	b.ReportAllocs()
	for b.Loop() {
		value, err := decodeRedisValue[uint64](source, "value")
		if err != nil || value != 18446744073709551615 {
			b.Fatalf("decode = %d, %v", value, err)
		}
	}
}
