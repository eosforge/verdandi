-- Unregister 把完整记录删除、成员索引删除和终止事件合并为一个原子操作。
-- KEYS[1] 是 Registration Hash，KEYS[2] 是 Registry 成员索引/频道，ARGV[1] 是 uuid。
local redis_call = redis.call
local registry_key = KEYS[2]
local uuid = ARGV[1]
local existed = redis_call("DEL", KEYS[1]) == 1
redis_call("HDEL", registry_key, uuid)

-- 只有完整记录确实存在时才发布终止事件；重复 Unregister 保持幂等且不制造虚假变化。
if existed then
    redis_call("PUBLISH", registry_key, cmsgpack.pack({
        "&protocol", "v1",
        "&kind", "unregister",
        "@uuid", uuid,
    }))
end
return { "&result", "ok" }
