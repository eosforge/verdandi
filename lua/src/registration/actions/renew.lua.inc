-- Renew 校验当前 revision，推进 Redis timestamp，并刷新 Registration Hash 与成员 field 两套 TTL。
-- ARGV[1] 是 uuid，ARGV[2] 是调用方最后确认的 revision；重写相同成员值也能修复意外缺失的索引 field。
local redis_call = redis.call
local to_number = tonumber
local registration_key = KEYS[1]
local registry_key = KEYS[2]
local uuid = ARGV[1]
local revision_value = ARGV[2]
local revision = to_number(revision_value)

-- 只读取续期和租约截止计算所需的 Redis 元字段，不读取 Attr/Data。
local state = redis_call(
    "HMGET", registration_key, "@revision", "@timestamp", "@ttl"
)
local stored_revision = state[1]
if not stored_revision then
    return { "&result", "error", "&status", "missing" }
end
local current_revision = to_number(stored_revision)
local current_timestamp = to_number(state[2])
local current_ttl = to_number(state[3])
if not current_revision or not current_timestamp or not current_ttl then
    return { "&result", "error", "&status", "corrupt" }
end

-- 热路径期望 revision 完全相同，仅用一次不等比较进入冲突分支。
if revision ~= current_revision then
    local status = "transition"
    if revision < current_revision then
        status = "stale"
    end
    return {
        "&result", "error",
        "&status", status,
        "&field", "@revision",
        "@revision", current_revision,
    }
end

-- Redis TIME 是租约权威时钟；保留记录中更大的 timestamp 可防止 Redis 时间短暂回拨缩短租约。
local redis_time = redis_call("TIME")
local timestamp = to_number(redis_time[1]) * 1000 +
    math.floor(to_number(redis_time[2]) / 1000)
if current_timestamp > timestamp then
    timestamp = current_timestamp
end
local deadline = timestamp + current_ttl
if deadline > 70368744177663 then
    return {
        "&result", "error",
        "&status", "invalid",
        "&field", "@ttl",
    }
end

-- Renew 不修改 Attr/Data/revision，因此事件只携带必要索引和新 timestamp。
local encoded_event = cmsgpack.pack({
    "&protocol", "v1",
    "&kind", "renew",
    "@uuid", uuid,
    "@revision", current_revision,
    "@timestamp", timestamp,
})
-- timestamp、Hash TTL、成员 field TTL 和 Pub/Sub 事件在一个脚本中原子提交。
redis_call("HSET", registration_key, "@timestamp", timestamp)
redis_call("PEXPIREAT", registration_key, deadline)
redis_call(
    "HSETEX", registry_key, "PXAT", deadline,
    "FIELDS", 1, uuid, revision_value
)
redis_call("PUBLISH", registry_key, encoded_event)
return { "&result", "ok", "@revision", current_revision, "@timestamp", timestamp }
