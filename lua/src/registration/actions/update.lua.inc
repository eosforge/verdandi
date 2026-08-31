-- Update 只比较 Redis 管理的 revision，再原子安装 SDK 已校验的 Data patch 并刷新租约。
-- arguments[1..3] 分别为 uuid、新 revision、version（不修改时为空），arguments[4..] 是已排序的局部 Data 键值对。
local uuid = arguments[1]
local revision_value = arguments[2]
local version_value = arguments[3]
local revision = to_number(revision_value)

-- timestamp/ttl 仅用于刷新租约；它们不是 Update 的用户可修改字段。
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

-- 热路径期望 revision 恰好递增一，仅用一次不等比较进入冲突分支。
if revision ~= current_revision + 1 then
    if revision == current_revision then
        return {
            "&result", "ok",
            "@revision", current_revision,
            "@timestamp", current_timestamp,
        }
    end
    if revision < current_revision then
        return {
            "&result", "error",
            "&status", "stale",
            "&field", "@revision",
            "@revision", current_revision,
        }
    end
    return {
        "&result", "error",
        "&status", "transition",
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

-- Update 事件只携带索引、时间、revision、可选 version 和本次局部 Data patch。
local event = {
    "&protocol", "v1",
    "&kind", "update",
    "@uuid", uuid,
    "@revision", revision,
    "@timestamp", timestamp,
}
local event_index = 11
if version_value ~= "" then
    event[11] = "@version"
    event[12] = to_number(version_value)
    event_index = 13
end
for index = 4, #arguments, 2 do
    event[event_index] = arguments[index]
    event[event_index + 1] = arguments[index + 1]
    event_index = event_index + 2
end
local encoded_event = cmsgpack.pack(event)

-- version 未变化时不写 @version，减少一个 Hash field 参数和一次内部字段覆盖。
if version_value == "" then
    redis_call(
        "HSET", registration_key,
        "@revision", revision_value,
        "@timestamp", timestamp,
        unpack(arguments, 4)
    )
else
    redis_call(
        "HSET", registration_key,
        "@revision", revision_value,
        "@timestamp", timestamp,
        "@version", version_value,
        unpack(arguments, 4)
    )
end
-- Data patch、Hash TTL、成员 field TTL 和 Pub/Sub 事件在一个脚本中原子提交。
redis_call("PEXPIREAT", registration_key, deadline)
redis_call(
    "HSETEX", registry_key, "PXAT", deadline,
    "FIELDS", 1, uuid, revision_value
)
redis_call("PUBLISH", registry_key, encoded_event)
return { "&result", "ok", "@revision", revision, "@timestamp", timestamp }
