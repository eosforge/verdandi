// Package teststats supplies fixed-memory statistics for qualification workloads.
package teststats

import (
	"math/bits"
	"time"
)

// Histogram retains counts, not individual samples. Quantiles return conservative
// bucket upper bounds with at most 1/16 relative bucket width above 16 ns.
// A caller owns synchronization; worker-local histograms can be merged after join.
type Histogram struct {
	Count   uint64
	sum     uint64
	maximum uint64
	zero    uint64
	buckets [1025]uint64
}

// Summary preserves exact counts/maximum/mean; percentiles are bucket estimates.
type Summary struct {
	Count       uint64        `json:"count"`
	P50         time.Duration `json:"p50_nanoseconds"`
	P95         time.Duration `json:"p95_nanoseconds"`
	P99         time.Duration `json:"p99_nanoseconds"`
	Maximum     time.Duration `json:"maximum_nanoseconds"`
	Average     time.Duration `json:"average_nanoseconds"`
	ZeroSamples uint64        `json:"zero_samples"`
}

// Observe records one nonnegative duration; negative scheduling lag is zero.
func (h *Histogram) Observe(value time.Duration) {
	n := uint64(max(value, 0))
	index := n
	if n >= 16 {
		exponent := bits.Len64(n) - 1
		index = uint64(exponent*16) + ((n - (uint64(1) << exponent)) >> (exponent - 4))
	}
	h.buckets[index]++
	h.Count++
	h.sum += n
	h.maximum = max(h.maximum, n)
	if n == 0 {
		h.zero++
	}
}

// Merge combines a completed worker without allocating or retaining its samples.
func (h *Histogram) Merge(other Histogram) {
	h.Count += other.Count
	h.sum += other.sum
	h.maximum = max(h.maximum, other.maximum)
	h.zero += other.zero
	for i, n := range other.buckets {
		h.buckets[i] += n
	}
}

// Quantile returns a nearest-rank conservative bucket bound, capped by the maximum.
func (h Histogram) Quantile(percent uint64) time.Duration {
	if h.Count == 0 {
		return 0
	}
	rank := (h.Count*min(percent, 100) + 99) / 100
	rank = max(rank, 1)
	var seen uint64
	for index, n := range h.buckets {
		seen += n
		if seen < rank {
			continue
		}
		upper := uint64(index)
		if index >= 64 {
			exponent, sub := index/16, index%16
			upper = (uint64(1) << exponent) + (uint64(sub+1) << (exponent - 4)) - 1
		}
		return time.Duration(min(upper, h.maximum))
	}
	return time.Duration(h.maximum)
}

// Snapshot computes a stable report from a completed or externally locked histogram.
func (h Histogram) Snapshot() Summary {
	result := Summary{Count: h.Count, P50: h.Quantile(50), P95: h.Quantile(95), P99: h.Quantile(99),
		Maximum: time.Duration(h.maximum), ZeroSamples: h.zero}
	if h.Count != 0 {
		result.Average = time.Duration(h.sum / h.Count)
	}
	return result
}
