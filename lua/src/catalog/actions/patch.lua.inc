local base_revision = ARGV[2]
local encoded_bytes_text = ARGV[3]
local set_count_text = ARGV[4]
local set_count = canonical_count(set_count_text, maximum_fields)
local encoded_bytes = canonical_count(encoded_bytes_text, maximum_encoded_bytes)
if #ARGV < 4 or not canonical_positive(base_revision) or not set_count or
    set_count == 0 or not encoded_bytes or #ARGV ~= 4 + set_count * 2 then
    return fail("contract")
end
if current_revision ~= base_revision or not live_revision then
    return fail("stale", "@base_revision", current_revision)
end
if current_kind == "value" then
    return fail("transition", "@kind", current_revision)
end
if global_revision == maximum_revision then
    return fail("capacity", "@revision")
end

local first_field = 5
local projected_bytes = tonumber(current_encoded_bytes)
local previous_name = nil
for index = 0, set_count - 1 do
    local name = ARGV[first_field + index * 2]
    local value = ARGV[first_field + index * 2 + 1]
    if not name or name == "" or string.sub(name, 1, 1) == "@" or not value then
        return fail("contract")
    end
    if current_kind == "array" then
        if name ~= "0" and not string.match(name, "^[1-9][0-9]*$") then
            return fail("transition", name, current_revision)
        end
    end
    if previous_name and previous_name >= name then
        return fail("contract")
    end
    previous_name = name
    local old = redis_call("HGET", catalog_key, name)
    if current_kind == "array" and not old then
        return fail("transition", name, current_revision)
    end
    if old then
        projected_bytes = projected_bytes + #value - #old
    else
        projected_bytes = projected_bytes + #name + #value
    end
end
if projected_bytes ~= encoded_bytes then
    return fail("contract", "@encoded_bytes", current_revision)
end

local next_revision = increment_decimal(global_revision)
local command_chunk_fields = 1024
redis_call(
    "HSET", catalog_key,
    "@revision", next_revision,
    "@encoded_bytes", encoded_bytes_text
)
for first = 0, set_count - 1, command_chunk_fields do
    local last = math.min(first + command_chunk_fields - 1, set_count - 1)
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

local sets = {}
for index = 0, set_count - 1 do
    sets[index * 2 + 1] = ARGV[first_field + index * 2]
    sets[index * 2 + 2] = ARGV[first_field + index * 2 + 1]
end
redis_call("PUBLISH", catalog_key, cmsgpack.pack({
    "v1", "patch", member, base_revision, next_revision,
    current_kind, encoded_bytes_text, sets
}))
return {
    "&result", "ok",
    "@revision", next_revision,
    "@floor_revision", floor_revision,
}
