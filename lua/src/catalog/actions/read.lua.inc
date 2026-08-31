if #KEYS ~= 5 or #ARGV ~= 2 then
    return error_reply("contract")
end

local live_key = KEYS[1]
local deleted_key = KEYS[2]
local deleted_time_key = KEYS[3]
local catalog_key = KEYS[4]
local field_revisions_key = KEYS[5]
local member = ARGV[1]
local local_revision = ARGV[2]

if not member or member == "" or not canonical_nonnegative(local_revision) then
    return error_reply("contract")
end
local live_type = redis_type(live_key)
local deleted_type = redis_type(deleted_key)
local deleted_time_type = redis_type(deleted_time_key)
local catalog_type = redis_type(catalog_key)
local field_revision_type = redis_type(field_revisions_key)
if (live_type ~= "none" and live_type ~= "zset") or
    (deleted_type ~= "none" and deleted_type ~= "zset") or
    (deleted_time_type ~= "none" and deleted_time_type ~= "zset") or
    (catalog_type ~= "none" and catalog_type ~= "hash") or
    (field_revision_type ~= "none" and field_revision_type ~= "zset") then
    return error_reply("corrupt")
end

local live_revision = redis_call("ZSCORE", live_key, member)
local deleted_revision = redis_call("ZSCORE", deleted_key, member)
local deleted_time = redis_call("ZSCORE", deleted_time_key, member)
if (live_revision and deleted_revision) or
    (live_revision and not canonical_positive(live_revision)) or
    (deleted_revision and not canonical_positive(deleted_revision)) or
    (deleted_time and not canonical_nonnegative(deleted_time)) then
    return error_reply("corrupt")
end
if (deleted_revision and not deleted_time) or
    (deleted_time and not deleted_revision) then
    return error_reply("corrupt")
end

if catalog_type == "none" then
    if live_revision or field_revision_type ~= "none" then
        return error_reply("corrupt")
    end
    if deleted_revision then
        return {
            "&result", "ok",
            "&status", "deleted",
            "@revision", deleted_revision,
        }
    end
    return {
        "&result", "ok",
        "&status", "absent",
        "@revision", "0",
    }
end

if not live_revision or deleted_revision or
    (field_revision_type == "none" and redis_call("HLEN", catalog_key) ~= 4) then
    return error_reply("corrupt")
end

local header = redis_call(
    "HMGET", catalog_key,
    "@revision", "@replace_revision", "@kind", "@encoded_bytes"
)
local revision = header[1]
local replace_revision = header[2]
local kind = header[3]
local encoded_bytes = header[4]
if not canonical_positive(revision) or
    not canonical_positive(replace_revision) or
    decimal_less(revision, replace_revision) or
    (kind ~= "value" and kind ~= "array" and kind ~= "map") or
    not canonical_nonnegative(encoded_bytes) or
    tonumber(encoded_bytes) > maximum_encoded_bytes or
    live_revision ~= revision then
    return error_reply("corrupt")
end

local mode = "replace"
local fields = {}
if local_revision == revision then
    mode = "unchanged"
elseif not decimal_less(local_revision, replace_revision) and
    decimal_less(local_revision, revision) then
    mode = "patch"
    local names = redis_call(
        "ZRANGEBYSCORE", field_revisions_key,
        "(" .. local_revision, revision
    )
    if #names > maximum_fields then
        return error_reply("corrupt")
    end
    for index = 1, #names do
        local name = names[index]
        local value = redis_call("HGET", catalog_key, name)
        if not value or name == "" or string.sub(name, 1, 1) == "@" then
            return error_reply("corrupt")
        end
        fields[#fields + 1] = name
        fields[#fields + 1] = value
    end
else
    local all = redis_call("HGETALL", catalog_key)
    local names = {}
    local values = {}
    for index = 1, #all, 2 do
        local name = all[index]
        local value = all[index + 1]
        if string.sub(name, 1, 1) == "@" then
            if name ~= "@revision" and name ~= "@replace_revision" and
                name ~= "@kind" and name ~= "@encoded_bytes" then
                return error_reply("corrupt")
            end
        else
            if name == "" then
                return error_reply("corrupt")
            end
            names[#names + 1] = name
            values[name] = value
        end
    end
    if #names > maximum_fields then
        return error_reply("corrupt")
    end
    table.sort(names)
    for index = 1, #names do
        local name = names[index]
        fields[#fields + 1] = name
        fields[#fields + 1] = values[name]
    end
end

return {
    "&result", "ok",
    "&status", "present",
    "&mode", mode,
    "@revision", revision,
    "@replace_revision", replace_revision,
    "@kind", kind,
    "@encoded_bytes", encoded_bytes,
    "&fields", fields,
}
