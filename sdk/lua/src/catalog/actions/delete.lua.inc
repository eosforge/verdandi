if #ARGV ~= 1 then
    return fail("contract")
end
if global_revision == maximum_revision then
    return fail("capacity", "@revision")
end

local retention_milliseconds = 86400000
local retention_count = 1000000
local prune_batch = 256
local next_revision = increment_decimal(global_revision)
local now = redis_call("TIME")
local now_milliseconds = tonumber(now[1]) * 1000 +
    math.floor(tonumber(now[2]) / 1000)

redis_call("UNLINK", catalog_key, field_revisions_key)
redis_call("ZREM", live_key, member)
redis_call("ZADD", deleted_key, next_revision, member)
redis_call("ZADD", deleted_time_key, now_milliseconds, member)

local evicted = {}
local selected = {}
local cutoff = now_milliseconds - retention_milliseconds
local expired = redis_call(
    "ZRANGEBYSCORE", deleted_time_key, "-inf", cutoff,
    "LIMIT", 0, prune_batch
)
for index = 1, #expired do
    local candidate = expired[index]
    if candidate ~= member and not selected[candidate] then
        selected[candidate] = true
        evicted[#evicted + 1] = candidate
    end
end

local excess = redis_call("ZCARD", deleted_key) - retention_count
local required = math.min(
    math.max(excess - #evicted, 0),
    prune_batch - #evicted
)
if required > 0 then
    local candidates = redis_call(
        "ZRANGE", deleted_key, 0, prune_batch * 2 - 1
    )
    for index = 1, #candidates do
        local candidate = candidates[index]
        if candidate ~= member and not selected[candidate] then
            selected[candidate] = true
            evicted[#evicted + 1] = candidate
            required = required - 1
            if required == 0 then
                break
            end
        end
    end
end

local next_floor = floor_revision
if #evicted > 0 then
    local revisions = redis_call("ZMSCORE", deleted_key, unpack(evicted))
    for index = 1, #revisions do
        local candidate_revision = revisions[index]
        if candidate_revision and decimal_less(next_floor, candidate_revision) then
            next_floor = candidate_revision
        end
    end
    redis_call("ZREM", deleted_key, unpack(evicted))
    redis_call("ZREM", deleted_time_key, unpack(evicted))
end

redis_call("HSET", meta_key, "@revision", next_revision,
    "@floor_revision", next_floor)
redis_call("PUBLISH", catalog_key, cmsgpack.pack({
    "v1", "delete", member, next_revision
}))
return {
    "&result", "ok",
    "@revision", next_revision,
    "@floor_revision", next_floor,
    "@pruned", tostring(#evicted),
}
