package teststats

import (
	"testing"
	"time"
	"unsafe"
)

func TestDurationHistogramBoundsAndMerge(t *testing.T) {
	var left, right Histogram
	if left.Snapshot().Count != 0 || left.Quantile(99) != 0 {
		t.Fatal("empty histogram")
	}
	for i := range 10000 {
		if i%2 == 0 {
			left.Observe(time.Duration(i))
		} else {
			right.Observe(time.Duration(i))
		}
	}
	left.Merge(right)
	value := left.Snapshot()
	if value.Count != 10000 || value.ZeroSamples != 1 || value.Maximum != 9999 || value.Average != 4999 {
		t.Fatalf("incorrect exact statistics: %+v", value)
	}
	for _, pair := range []struct {
		percent  uint64
		expected time.Duration
	}{{50, 4999}, {95, 9499}, {99, 9899}} {
		actual := left.Quantile(pair.percent)
		if actual < pair.expected || actual > pair.expected+pair.expected/16+1 {
			t.Fatalf("percentile %d: %v outside conservative bound for %v", pair.percent, actual, pair.expected)
		}
	}
	if unsafe.Sizeof(left) > 9*1024 {
		t.Fatal("histogram memory grew")
	}
}

func TestDurationHistogramExtremes(t *testing.T) {
	for _, value := range []time.Duration{0, 1, 15, 16, 17, 31, 32, 1 << 62, 1<<63 - 1} {
		var histogram Histogram
		histogram.Observe(value)
		if histogram.Quantile(100) != value || histogram.Snapshot().Average != value {
			t.Fatalf("boundary %v", value)
		}
	}
}
