package registration

import (
	"context"
	_ "embed"

	redis "github.com/redis/go-redis/v9"
)

// 以下生成脚本必须与 lua/registration 下已审查的规范产物逐字节一致；不得手工修改嵌入副本。
//
//go:embed internal/protocol/register.lua
var registrationRegisterLua string

//go:embed internal/protocol/update.lua
var registrationUpdateLua string

//go:embed internal/protocol/renew.lua
var registrationRenewLua string

//go:embed internal/protocol/unregister.lua
var registrationUnregisterLua string

// registrationScriptKind 标识四种互不兼容的固定位置 Lua ABI。
type registrationScriptKind uint8

const (
	registrationScriptRegister registrationScriptKind = iota + 1
	registrationScriptUpdate
	registrationScriptRenew
	registrationScriptUnregister
)

// registrationScripts 保存四个独立脚本对象及其 SHA 缓存。
type registrationScripts struct {
	register   *redis.Script
	update     *redis.Script
	renew      *redis.Script
	unregister *redis.Script
}

// protocolScripts 是由只读嵌入文本构造的进程级脚本集合；redis.Script 可并发安全复用。
var protocolScripts = registrationScripts{
	register:   redis.NewScript(registrationRegisterLua),
	update:     redis.NewScript(registrationUpdateLua),
	renew:      redis.NewScript(registrationRenewLua),
	unregister: redis.NewScript(registrationUnregisterLua),
}

// script 按 kind 选择唯一脚本；未知 kind 返回 nil，由调用方转换为稳定 invalid 错误。
func (scripts registrationScripts) script(kind registrationScriptKind) *redis.Script {
	switch kind {
	case registrationScriptRegister:
		return scripts.register
	case registrationScriptUpdate:
		return scripts.update
	case registrationScriptRenew:
		return scripts.renew
	case registrationScriptUnregister:
		return scripts.unregister
	default:
		return nil
	}
}

// load 用一个 Pipeline 预加载全部 Registration 脚本并建立 SHA 缓存。
// ctx 控制整批加载；任一脚本失败都返回 Pipeline 执行错误。
func (scripts registrationScripts) load(ctx context.Context, client *redis.Client) error {
	pipeline := client.Pipeline()
	scripts.register.Load(ctx, pipeline)
	scripts.update.Load(ctx, pipeline)
	scripts.renew.Load(ctx, pipeline)
	scripts.unregister.Load(ctx, pipeline)
	_, err := pipeline.Exec(ctx)
	return err
}
