local kind = ARGV[2]
local encoded_bytes_text = ARGV[3]
local field_count_text = ARGV[4]
local field_count = canonical_count(field_count_text, maximum_fields)
local encoded_bytes = canonical_count(encoded_bytes_text, maximum_encoded_bytes)
if #ARGV < 4 or not field_count or not encoded_bytes or
    #ARGV ~= 4 + field_count * 2 or
    (kind ~= "value" and kind ~= "array" and kind ~= "map") or
    (kind == "value" and field_count ~= 1) then
    return fail("contract")
end

local first_field = 5
local calculated_bytes = 0
local previous_name = nil
for index = 0, field_count - 1 do
    local name = ARGV[first_field + index * 2]
    local value = ARGV[first_field + index * 2 + 1]
    if not name or name == "" or string.sub(name, 1, 1) == "@" or not value then
        return fail("contract")
    end
    if kind == "value" then
        if name ~= "value" then
            return fail("contract")
        end
    elseif kind == "array" then
        if name ~= tostring(index) then
            return fail("contract")
        end
    elseif previous_name and previous_name >= name then
        return fail("contract")
    end
    previous_name = name
    calculated_bytes = calculated_bytes + #name + #value
end
if calculated_bytes ~= encoded_bytes then
    return fail("contract", "@encoded_bytes")
end
if global_revision == maximum_revision then
    return fail("capacity", "@revision")
end

local next_revision = increment_decimal(global_revision)
local command_chunk_fields = 1024
redis_call("UNLINK", catalog_key, field_revisions_key)
redis_call(
    "HSET", catalog_key,
    "@revision", next_revision,
    "@replace_revision", next_revision,
    "@kind", kind,
    "@encoded_bytes", encoded_bytes_text
)
for first = 0, field_count - 1, command_chunk_fields do
    local last = math.min(first + command_chunk_fields - 1, field_count - 1)
    redis_call(
        "HSET", catalog_key,
        unpack(ARGV, first_field + first * 2, first_field + last * 2 + 1)
    )
    local revisions = {}
    for index = first, last do
        revisions[#revisions + 1] = next_revision
        revisions[#revisions + 1] = ARGV[first_field + index * 2]
    end
    redis_call("ZADD", field_revisions_key, unpack(revisions))
end

redis_call("HSET", meta_key, "@revision", next_revision,
    "@floor_revision", floor_revision)
redis_call("ZADD", live_key, next_revision, member)
redis_call("ZREM", deleted_key, member)
redis_call("ZREM", deleted_time_key, member)

local fields = {}
for index = 0, field_count - 1 do
    fields[index * 2 + 1] = ARGV[first_field + index * 2]
    fields[index * 2 + 2] = ARGV[first_field + index * 2 + 1]
end
redis_call("PUBLISH", catalog_key, cmsgpack.pack({
    "v1", "replace", member, next_revision, kind, encoded_bytes_text, fields
}))
return {
    "&result", "ok",
    "@revision", next_revision,
    "@floor_revision", floor_revision,
}
