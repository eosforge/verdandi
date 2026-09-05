package validate

import (
	"strings"
	"testing"
	"time"
)

func TestUintDecimalIsCanonicalAndBoundedForBothInputs(t *testing.T) {
	t.Parallel()

	for _, test := range []struct {
		name      string
		value     string
		maximum   uint64
		allowZero bool
		want      uint64
		valid     bool
	}{
		{name: "zero allowed", value: "0", allowZero: true, valid: true},
		{name: "zero rejected", value: "0"},
		{name: "maximum", value: "18446744073709551615", maximum: ^uint64(0), want: ^uint64(0), valid: true},
		{name: "overflow", value: "18446744073709551616", maximum: ^uint64(0)},
		{name: "bounded", value: "255", maximum: 255, want: 255, valid: true},
		{name: "above bound", value: "256", maximum: 255},
		{name: "empty", value: "", maximum: 255},
		{name: "leading zero", value: "01", maximum: 255},
		{name: "plus", value: "+1", maximum: 255},
		{name: "minus", value: "-1", maximum: 255},
		{name: "non-digit", value: "1x", maximum: 255},
	} {
		t.Run(test.name, func(t *testing.T) {
			fromString, stringOK := UintDecimal(test.value, test.maximum, test.allowZero)
			fromBytes, bytesOK := UintDecimalBytes([]byte(test.value), test.maximum, test.allowZero)
			if stringOK != test.valid || bytesOK != test.valid || fromString != test.want || fromBytes != test.want {
				t.Fatalf(
					"UintDecimal(%q, %d, %t) = string(%d,%t) bytes(%d,%t), want (%d,%t)",
					test.value, test.maximum, test.allowZero,
					fromString, stringOK, fromBytes, bytesOK, test.want, test.valid,
				)
			}
		})
	}
}

func TestDurationUsesDefaultAndEnforcesExactClosedRange(t *testing.T) {
	const (
		fallback = 2 * time.Second
		minimum  = time.Second
		maximum  = 3 * time.Second
	)
	tests := []struct {
		name  string
		value time.Duration
		want  time.Duration
		ok    bool
	}{
		{name: "default", want: fallback, ok: true},
		{name: "minimum", value: minimum, want: minimum, ok: true},
		{name: "maximum", value: maximum, want: maximum, ok: true},
		{name: "below", value: minimum - time.Millisecond},
		{name: "above", value: maximum + time.Millisecond},
		{name: "sub-millisecond", value: minimum + time.Nanosecond},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			got, ok := Duration(test.value, fallback, minimum, maximum)
			if got != test.want || ok != test.ok {
				t.Fatalf("Duration(%s) = (%s, %t), want (%s, %t)", test.value, got, ok, test.want, test.ok)
			}
		})
	}
}

func TestOptionalValuesDistinguishDefaultFromExplicitZero(t *testing.T) {
	durationFallback := 2 * time.Second
	if got, ok := OptionalDuration(nil, durationFallback, 0, 3*time.Second); got != durationFallback || !ok {
		t.Fatalf("OptionalDuration(nil) = (%s, %t)", got, ok)
	}
	zeroDuration := time.Duration(0)
	if got, ok := OptionalDuration(&zeroDuration, durationFallback, 0, 3*time.Second); got != 0 || !ok {
		t.Fatalf("OptionalDuration(&0) = (%s, %t)", got, ok)
	}
	fractional := time.Second + time.Nanosecond
	if got, ok := OptionalDuration(&fractional, durationFallback, 0, 3*time.Second); got != 0 || ok {
		t.Fatalf("OptionalDuration(sub-millisecond) = (%s, %t)", got, ok)
	}

	if got, ok := OptionalInt(nil, 2, 0, 3); got != 2 || !ok {
		t.Fatalf("OptionalInt(nil) = (%d, %t)", got, ok)
	}
	zeroInt := 0
	if got, ok := OptionalInt(&zeroInt, 2, 0, 3); got != 0 || !ok {
		t.Fatalf("OptionalInt(&0) = (%d, %t)", got, ok)
	}
	below := -1
	if got, ok := OptionalInt(&below, 2, 0, 3); got != 0 || ok {
		t.Fatalf("OptionalInt(below) = (%d, %t)", got, ok)
	}
	above := 4
	if got, ok := OptionalInt(&above, 2, 0, 3); got != 0 || ok {
		t.Fatalf("OptionalInt(above) = (%d, %t)", got, ok)
	}
}

func TestZoneAcceptsOnlyOneToThirtyTwoASCIILetters(t *testing.T) {
	tests := []struct {
		name string
		zone string
		want bool
	}{
		{name: "single-lower", zone: "a", want: true},
		{name: "single-upper", zone: "Z", want: true},
		{name: "mixed-maximum", zone: strings.Repeat("a", 16) + strings.Repeat("Z", 16), want: true},
		{name: "empty"},
		{name: "too-long", zone: strings.Repeat("a", 33)},
		{name: "digit", zone: "Zone1"},
		{name: "separator", zone: "Zone-East"},
		{name: "non-ascii", zone: "区域"},
		{name: "nul", zone: "A\x00B"},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			if got := Zone(test.zone); got != test.want {
				t.Fatalf("Zone(%q) = %t, want %t", test.zone, got, test.want)
			}
		})
	}
}
