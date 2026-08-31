//go:build !windows

package verdandi

import "go.uber.org/goleak"

func platformGoleakOptions() []goleak.Option {
	return nil
}
