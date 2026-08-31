local redis_call = redis.call

local function redis_type(key)
    local result = redis_call("TYPE", key)
    if type(result) == "table" then
        return result.ok
    end
    return result
end

local function error_reply(status, field, revision)
    local result = { "&result", "error", "&status", status }
    if field then
        result[#result + 1] = "&field"
        result[#result + 1] = field
    end
    if revision then
        result[#result + 1] = "@revision"
        result[#result + 1] = revision
    end
    return result
end
