//go:build !windows

package registration

import "go.uber.org/goleak"

func platformGoleakOptions() []goleak.Option {
	return nil
}
