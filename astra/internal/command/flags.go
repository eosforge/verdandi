// Package command 保留 Go 标准 flag 行为, 只补充部署参数不允许重复覆盖的约束.
package command

import (
	"errors"
	"flag"
)

type once struct {
	flag.Value
	set bool
}

func (value *once) String() string {
	if value.Value == nil {
		return "" // flag 的帮助生成会反射构造零值, 不能调用空接口的 String.
	}
	return value.Value.String()
}

func (value *once) Set(text string) error {
	if value.set {
		return errors.New("duplicate option")
	}
	value.set = true
	return value.Value.Set(text)
}

func (value *once) IsBoolFlag() bool {
	boolean, ok := value.Value.(interface{ IsBoolFlag() bool })
	return ok && boolean.IsBoolFlag()
}

// Parse 不安装工具或执行服务, 只在调用前包装已注册参数; 每个 FlagSet 仅解析一次.
func Parse(flags *flag.FlagSet, arguments []string) error {
	flags.VisitAll(func(option *flag.Flag) { option.Value = &once{Value: option.Value} })
	return flags.Parse(arguments)
}
