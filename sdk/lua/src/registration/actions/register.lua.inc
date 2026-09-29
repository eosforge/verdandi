-- Register 安装 SDK 已完整校验的 Attr/Data 状态；Lua 只负责阻止旧 revision 的全量状态覆盖较新记录。
-- arguments[1..4] 分别为 uuid、revision、ttl_ms、version，arguments[5..] 是已排序的 Attr/Data 字段键值对。
local uuid = arguments[1]
local revision_value = arguments[2]
local ttl_value = arguments[3]
local version_value = arguments[4]
local revision = to_number(revision_value)
local ttl = to_number(ttl_value)
local version = to_number(version_value)

-- 只读取覆盖裁决需要的 Redis 元字段；数据形状和字段限制已经由 SDK 在请求入队前校验。
local previous = redis_call("HMGET", registration_key, "@revision", "@timestamp")
local previous_revision = previous[1]
local previous_timestamp = previous[2]
local current_revision = to_number(previous_revision)
local current_timestamp = to_number(previous_timestamp)
if (previous_revision and not current_revision) or
    (previous_timestamp and not current_timestamp) then
    return { "&result", "error", "&status", "corrupt" }
end
if current_revision and revision < current_revision then
    return {
        "&result", "error",
        "&status", "stale",
        "&field", "@revision",
        "@revision", current_revision,
    }
end

-- Redis TIME 是租约权威时钟；保留记录中更大的 timestamp 可防止 Redis 时间短暂回拨缩短租约。
local redis_time = redis_call("TIME")
local timestamp = to_number(redis_time[1]) * 1000 +
    math.floor(to_number(redis_time[2]) / 1000)
if current_timestamp and current_timestamp > timestamp then
    timestamp = current_timestamp
end
local deadline = timestamp + ttl
if deadline > 70368744177663 then
    return {
        "&result", "error",
        "&status", "invalid",
        "&field", "@ttl",
    }
end

-- 事件包含 Selector 重建全量记录所需的全部字段；Lua 仅拼装固定位置，不解析用户字节。
local event = {
    "&protocol", "v1",
    "&kind", "register",
    "@uuid", uuid,
    "@revision", revision,
    "@timestamp", timestamp,
    "@ttl", ttl,
    "@version", version,
}
local event_index = 15
for index = 5, #arguments, 2 do
    event[event_index] = arguments[index]
    event[event_index + 1] = arguments[index + 1]
    event_index = event_index + 2
end
local encoded_event = cmsgpack.pack(event)

-- 已存在的有效记录需要先清空以删除旧字段；首次/恢复 Register 避免对不存在的 key 额外发送 DEL。
if previous_revision or previous_timestamp then
    redis_call("DEL", registration_key)
end
-- 完整 Hash、Hash TTL、成员 field TTL 和 Pub/Sub 事件在一个脚本中原子提交。
redis_call(
    "HSET",
    registration_key,
    "@uuid", uuid,
    "@revision", revision_value,
    "@timestamp", timestamp,
    "@ttl", ttl_value,
    "@version", version_value,
    unpack(arguments, 5)
)
redis_call("PEXPIREAT", registration_key, deadline)
redis_call(
    "HSETEX", registry_key, "PXAT", deadline,
    "FIELDS", 1, uuid, revision_value
)
redis_call("PUBLISH", registry_key, encoded_event)
return { "&result", "ok", "@revision", revision, "@timestamp", timestamp }
