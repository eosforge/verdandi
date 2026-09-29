local maximum_revision = "9007199254740991"
local maximum_encoded_bytes = 4194304
local maximum_fields = 65536

local function canonical_nonnegative(value)
    if value == "0" then
        return true
    end
    if not value or not string.match(value, "^[1-9][0-9]*$") then
        return false
    end
    return #value < #maximum_revision or
        (#value == #maximum_revision and value <= maximum_revision)
end

local function canonical_positive(value)
    return value ~= "0" and canonical_nonnegative(value)
end

local function decimal_less(left, right)
    if #left ~= #right then
        return #left < #right
    end
    return left < right
end

local function increment_decimal(value)
    local length = #value
    local index = length
    while index > 0 and string.byte(value, index) == 57 do
        index = index - 1
    end
    if index == 0 then
        return "1" .. string.rep("0", length)
    end
    return string.sub(value, 1, index - 1) ..
        string.char(string.byte(value, index) + 1) ..
        string.rep("0", length - index)
end

local function canonical_count(value, maximum)
    if value ~= "0" and
        (not value or not string.match(value, "^[1-9][0-9]*$")) then
        return nil
    end
    local result = tonumber(value)
    if not result or result > maximum or result % 1 ~= 0 then
        return nil
    end
    return result
end
