local meta_key = KEYS[1]
local live_key = KEYS[2]
local deleted_key = KEYS[3]
local deleted_time_key = KEYS[4]
local catalog_key = KEYS[5]
local field_revisions_key = KEYS[6]
local member = ARGV[1]

if #KEYS ~= 6 or #ARGV < 1 or not member or member == "" then
    return error_reply("contract")
end

local fail = error_reply

local meta_type = redis_type(meta_key)
local live_type = redis_type(live_key)
local deleted_type = redis_type(deleted_key)
local deleted_time_type = redis_type(deleted_time_key)
local catalog_type = redis_type(catalog_key)
local field_revision_type = redis_type(field_revisions_key)
if (meta_type ~= "none" and meta_type ~= "hash") or
    (live_type ~= "none" and live_type ~= "zset") or
    (deleted_type ~= "none" and deleted_type ~= "zset") or
    (deleted_time_type ~= "none" and deleted_time_type ~= "zset") or
    (catalog_type ~= "none" and catalog_type ~= "hash") or
    (field_revision_type ~= "none" and field_revision_type ~= "zset") then
    return fail("corrupt")
end

local meta = redis_call("HMGET", meta_key, "@revision", "@floor_revision")
local global_revision = meta[1]
local floor_revision = meta[2]
if not global_revision then
    if floor_revision or redis_call("HLEN", meta_key) ~= 0 or
        redis_call("ZCARD", live_key) ~= 0 or
        redis_call("ZCARD", deleted_key) ~= 0 or
        redis_call("ZCARD", deleted_time_key) ~= 0 then
        return fail("corrupt")
    end
    global_revision = "0"
    floor_revision = "0"
elseif not canonical_positive(global_revision) or
    not canonical_nonnegative(floor_revision) or
    decimal_less(global_revision, floor_revision) then
    return fail("corrupt")
end

local live_revision = redis_call("ZSCORE", live_key, member)
local deleted_revision = redis_call("ZSCORE", deleted_key, member)
local deleted_time = redis_call("ZSCORE", deleted_time_key, member)
if (live_revision and deleted_revision) or
    ((deleted_revision and not deleted_time) or
        (deleted_time and not deleted_revision)) or
    (live_revision and not canonical_positive(live_revision)) or
    (deleted_revision and not canonical_positive(deleted_revision)) or
    (deleted_time and not canonical_nonnegative(deleted_time)) then
    return fail("corrupt")
end

local current_revision = "0"
local current_kind = nil
local current_encoded_bytes = "0"
local current_replace_revision = "0"

if catalog_type == "hash" then
    if not live_revision or deleted_revision then
        return fail("corrupt")
    end
    local header = redis_call(
        "HMGET", catalog_key,
        "@revision", "@replace_revision", "@kind", "@encoded_bytes"
    )
    current_revision = header[1]
    current_replace_revision = header[2]
    current_kind = header[3]
    current_encoded_bytes = header[4]
    if not canonical_positive(current_revision) or
        not canonical_positive(current_replace_revision) or
        decimal_less(current_revision, current_replace_revision) or
        (current_kind ~= "value" and current_kind ~= "array" and
            current_kind ~= "map") or
        not canonical_nonnegative(current_encoded_bytes) or
        tonumber(current_encoded_bytes) > maximum_encoded_bytes or
        live_revision ~= current_revision then
        return fail("corrupt")
    end
    if field_revision_type == "none" and redis_call("HLEN", catalog_key) ~= 4 then
        return fail("corrupt")
    end
elseif live_revision or field_revision_type == "zset" then
    return fail("corrupt")
elseif deleted_revision then
    current_revision = deleted_revision
end

if decimal_less(global_revision, current_revision) then
    return fail("corrupt")
end
