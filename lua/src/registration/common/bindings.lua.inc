-- 缓存每条热路径都会反复使用的运行时函数、参数表和 key，避免重复全局/表查找。
local redis_call = redis.call
local to_number = tonumber
-- arguments 是当前命令完整 ARGV；action fragment 按固定位置解释控制字段和用户字段键值对。
local arguments = ARGV
-- registration_key 保存一个进程内 Registration 的完整 Hash；registry_key 保存同 zone/type 的成员 revision 并兼作 Pub/Sub channel。
local registration_key = KEYS[1]
local registry_key = KEYS[2]
