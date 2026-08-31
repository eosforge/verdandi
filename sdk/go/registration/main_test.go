package registration

import (
	"testing"

	"go.uber.org/goleak"
)

func TestMain(main *testing.M) {
	options := []goleak.Option{goleak.IgnoreCurrent()}
	options = append(options, platformGoleakOptions()...)
	goleak.VerifyTestMain(main, options...)
}
