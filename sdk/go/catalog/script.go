package catalog

import (
	"context"
	_ "embed"

	redis "github.com/redis/go-redis/v9"
)

// 下列嵌入脚本必须与 lua/catalog 生成输出逐字节一致，禁止直接修改副本。

//go:embed internal/protocol/read.lua
var readLua string

//go:embed internal/protocol/replace.lua
var replaceLua string

//go:embed internal/protocol/patch.lua
var patchLua string

//go:embed internal/protocol/delete.lua
var deleteLua string

type scripts struct {
	read    *redis.Script
	replace *redis.Script
	patch   *redis.Script
	delete  *redis.Script
}

// newScripts 从嵌入的规范 Lua 文本创建一组可并发复用的 redis.Script。
func newScripts() scripts {
	return scripts{
		read:    redis.NewScript(readLua),
		replace: redis.NewScript(replaceLua),
		patch:   redis.NewScript(patchLua),
		delete:  redis.NewScript(deleteLua),
	}
}

// load 用 Pipeline 预加载全部 Catalog 脚本；ctx 控制整批执行。
func (scripts scripts) load(ctx context.Context, client *redis.Client) error {
	pipeline := client.Pipeline()
	scripts.read.Load(ctx, pipeline)
	scripts.replace.Load(ctx, pipeline)
	scripts.patch.Load(ctx, pipeline)
	scripts.delete.Load(ctx, pipeline)
	_, err := pipeline.Exec(ctx)
	return err
}
