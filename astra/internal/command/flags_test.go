package command

import (
	"errors"
	"flag"
	"io"
	"testing"
)

// 参数包装必须保留标准布尔语义/帮助, 同时拒绝重复值的静默覆盖.
func TestParse(t *testing.T) {
	for _, test := range []struct {
		args []string
		bad  bool
	}{
		{[]string{"--port=1", "--enabled"}, false},
		{[]string{"--port", "1", "--enabled=false"}, false},
		{[]string{"--port=1", "--port=2"}, true},
		{[]string{"--enabled", "--enabled=false"}, true},
		{[]string{"--absent=1"}, true},
	} {
		flags := flag.NewFlagSet("test", flag.ContinueOnError)
		flags.SetOutput(io.Discard)
		flags.Int("port", 0, "test port")
		flags.Bool("enabled", false, "test switch")
		if err := Parse(flags, test.args); (err != nil) != test.bad {
			t.Fatalf("parse %v: unexpected result", test.args)
		}
	}
	flags := flag.NewFlagSet("help", flag.ContinueOnError)
	flags.SetOutput(io.Discard)
	flags.Bool("enabled", false, "test switch")
	flags.String("name", "value", "test text")
	if !errors.Is(Parse(flags, []string{"--help"}), flag.ErrHelp) {
		t.Fatal("standard help lost")
	}
}
