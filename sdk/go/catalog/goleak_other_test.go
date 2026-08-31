//go:build !windows

package catalog

import "go.uber.org/goleak"

func platformGoleakOptions() []goleak.Option {
	return nil
}
