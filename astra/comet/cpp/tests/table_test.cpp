#include "check.hpp"
#include "table.hpp"
#include <array>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
// 用实际拥有的正文验证旧根, 另允许伪造逻辑长度触发溢出而不申请巨大内存.
struct Record {
    static constexpr std::size_t overhead = sizeof(std::string) + 32; // 测试记录的正文对象及共享控制块估算.
    std::shared_ptr<const std::string> value;                         // 正常记录始终非空, 旧根独立保有正文.
    std::size_t length{};                                             // 正常等于 value->size(), 溢出用例可设置 SIZE_MAX.

    // 与真实三域记录相同的逻辑字节计量入口.
    std::size_t bytes() const noexcept {
        return length;
    }
};

using Table = comet::detail::Table<Record>; // 只测试私有拥有式页, 不引入网络或业务版本.

// 返回具有独立正文的记录; 调用方可保留弱引用观察候选释放.
Record record(std::string value) {
    const auto length = value.size(); // 移交字符串之前保存真实长度.
    return {std::make_shared<const std::string>(std::move(value)), length};
}

// 选择四个位图字的首尾页, 测试不假定具体 Key 的实现相关哈希值.
std::vector<std::string> boundaries() {

    constexpr std::array<std::size_t, 8> targets{0, 63, 64, 127, 128, 191, 192, 255}; // 跨 64 位边界及最高页.
    std::array<std::string, targets.size()> found{};                                  // 每个目标只保留第一个命中的非空 Key.
    std::size_t remaining = found.size();                                             // 有界搜索的剩余页数, 找齐即停.
    for (unsigned attempt = 0; attempt < 262144 && remaining != 0; ++attempt) {
        auto key = "boundary-" + std::to_string(attempt); // 与 Table 同样以 string_view 哈希定位页.
        const auto slot = std::hash<std::string_view>{}(key) & 255;
        for (std::size_t index = 0; index < targets.size(); ++index) {
            if (targets[index] == slot && found[index].empty()) {
                found[index] = std::move(key);
                --remaining;
                break;
            }
        }
    }
    CHECK(remaining == 0);
    return {found.begin(), found.end()};
}

// 位图跨字发布、删除最后一项及再次使用空页, 不能遗漏首尾页或修改旧根.
void pages() {

    const auto keys = boundaries(); // 八个独立页, 同时覆盖全部四个位图字.
    Table current;                  // 初始无页, 空间只含根对象.
    CHECK(current.footprint() == sizeof(Table));
    Table::Draft initial(current);
    for (const auto& key : keys) {
        initial.set(key, record("before"));
    }
    current = std::move(initial).finish();
    const auto old = current;              // 后续覆盖和清空不能改变这份共享根.
    const auto retained = old.footprint(); // 原根计量同样不可变化.
    CHECK(current.size() == keys.size());

    Table::Draft changed(current);
    for (std::size_t index = 0; index < keys.size(); ++index) {
        if (index % 2 == 0) {
            changed.erase(keys[index]);
        } else {
            changed.set(keys[index], record("after"));
        }
    }
    Table::Draft moved(std::move(changed)); // 移交候选时修改位图、计量和页面必须一起迁移.
    current = std::move(moved).finish();
    CHECK(current.size() == keys.size() / 2);
    for (std::size_t index = 0; index < keys.size(); ++index) {
        CHECK(old.find(keys[index]) && *old.find(keys[index])->value == "before");
        const auto* item = current.find(keys[index]); // 只借用当前完整根, 不保留到下一次赋值.
        CHECK(index % 2 == 0 ? item == nullptr : item && *item->value == "after");
    }
    CHECK(old.footprint() == retained);

    Table::Draft cleared(current);
    for (const auto& key : keys) {
        cleared.erase(key); // 同时覆盖已缺失键的无操作删除.
    }
    current = std::move(cleared).finish();
    CHECK(current.size() == 0 && current.bytes() == 0 && current.footprint() == sizeof(Table));
    Table::Draft reused(current);
    reused.set(keys.back(), record("temporary"));
    reused.erase(keys.back()); // 同一候选新增后立即删除, 发布必须归还空页和桶计量.
    current = std::move(reused).finish();
    CHECK(current.footprint() == sizeof(Table));
}

// 持续增删改与独立有序表核对, 并验证大幅缩容后不留下空桶计费或旧根变化.
void contents() {

    Table current;                               // 被测分页表.
    std::map<std::string, std::string> expected; // 独立参考模型, 不复写分页或空间计量算法.
    for (unsigned round = 0; round < 24; ++round) {
        const auto old = current;               // 每批都持有旧根, 迫使候选保持拥有式 COW.
        const auto previous = expected;         // 用于验证旧根未被新批改变.
        const auto footprint = old.footprint(); // 重复读取旧根计量必须稳定.
        Table::Draft draft(current);
        for (unsigned index = 0; index < 192; ++index) {
            auto key = "entry-" + std::to_string((round * 73 + index) % 768); // 固定序列覆盖重复覆盖和删除.
            if ((round + index) % 4 == 0) {
                draft.erase(key);
                expected.erase(key);
            } else {
                auto value = std::to_string(round) + ":" + std::to_string(index);
                draft.set(key, record(value));
                expected.insert_or_assign(std::move(key), std::move(value));
            }
        }
        current = std::move(draft).finish();
        CHECK(current.size() == expected.size() && old.size() == previous.size() && old.footprint() == footprint);
        std::size_t bytes{}; // 从外部可见项独立汇总逻辑大小, 不访问缓存字段.
        std::size_t count{}; // 验证遍历没有遗漏、重复或遗留项.
        current.each([&](std::string_view key, const Record& item) {
            CHECK(expected.at(std::string(key)) == *item.value);
            bytes += key.size() + item.value->size();
            ++count;
        });
        CHECK(bytes == current.bytes() && count == expected.size() && current.footprint() >= bytes + sizeof(Table));
        for (const auto& [key, value] : previous) {
            CHECK(old.find(key) && *old.find(key)->value == value);
        }
    }

    Table::Draft cleared(current);
    for (const auto& [key, value] : expected) {
        static_cast<void>(value); // 删除只需要参考表 Key.
        cleared.erase(key);
    }
    current = std::move(cleared).finish();
    CHECK(current.footprint() == sizeof(Table));
}

// 新增只借用输入直到返回, 页内必须拥有名称; 覆盖、重复删除和重新新增保持计量一致.
void keys() {

    Table current; // 空根不拥有调用方后续复用的名称缓存.
    Table::Draft draft(current);
    std::string key(256, 'a'); // 长 Key 强制离开 SSO, 同一缓存会立即被覆盖和释放.
    const auto original = key; // 独立参考名称, 用于后续点查.
    draft.set(key, record("first"));
    key.assign("short");
    draft.set(key, record("second"));
    key.clear();
    key.shrink_to_fit();
    current = std::move(draft).finish();
    const auto old = current; // 新批次不得改写旧根拥有的名称或正文.
    CHECK(current.size() == 2 && *current.find(original)->value == "first" && *current.find("short")->value == "second");

    Table::Draft changed(current);
    changed.set(original, record("updated"));
    changed.set(original, record("final")); // 同一已独占页再次覆盖, 不增加记录数或重复计费.
    changed.erase(original);
    changed.erase(original); // 已独占页上的缺失 Delete 不得下溢计数.
    changed.set(original, record("restored"));
    current = std::move(changed).finish();
    CHECK(current.size() == 2 && current.bytes() == original.size() + 8 + 5 + 6);
    CHECK(*current.find(original)->value == "restored" && *old.find(original)->value == "first");
}

// 放弃候选和长度溢出不泄漏临时正文, 不改变已发布内容或计量.
void rollback() {

    Table current;
    Table::Draft initial(current);
    initial.set("key", record("retained"));
    current = std::move(initial).finish();
    const auto footprint = current.footprint(); // 两种失败都必须保持该完整根.
    std::weak_ptr<const std::string> temporary; // 不延长候选正文寿命.
    {
        Table::Draft abandoned(current);
        auto value = record("temporary");
        temporary = value.value;
        abandoned.set("key", std::move(value));
    }
    CHECK(temporary.expired() && *current.find("key")->value == "retained" && current.footprint() == footprint);
    bool rejected{}; // 只接收预期的计量溢出, 其他异常由测试入口报告.
    try {
        Table::Draft failed(current);
        auto value = record("bounded allocation");
        value.length = std::numeric_limits<std::size_t>::max();
        failed.set("overflow", std::move(value));
    } catch (const std::length_error&) {
        rejected = true;
    }
    CHECK(rejected && !current.find("overflow") && current.size() == 1 && current.footprint() == footprint);
}
} // namespace

// 纯拥有式字典用例, 不创建线程、网络或工具进程.
int main() {
    try {
        pages();
        contents();
        keys();
        rollback();
        std::cout << "Comet table cases passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
