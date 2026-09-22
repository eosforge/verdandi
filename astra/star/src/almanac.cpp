#include "almanac.hpp"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <utility>

namespace astra {
// 一个 Scope 的原生状态. Map 只负责点查, Index 负责共享分页视图, 两者共用 Key/Value 所有权.
struct Almanac::State {
    // 可变点查入口, 不含期限、时间轮节点、发布者或第二份业务版本.
    struct Entry {
        // 拥有 Map 的 string_view 所借用的不可变字符串, 不依赖请求或 Proto 的寿命.
        std::shared_ptr<const std::string> key;
        // 当前完整字节, 与页索引/历史共享; 临时新节点在提交前可以为空.
        Value value;
        // Index 中的紧凑槽号, 删除后由 Index::next 复用, 不充当 Key 身份.
        std::uint64_t slot{};
    };

    // 只以原始 Key 字节定位, 不拼接 Scope 路径或为不存在的读取建立条目.
    std::unordered_map<std::string_view, Entry> entries;
    // 复用已存在的页复制机制, 不复用 Store 的版本、TTL 或提交语义; deadline 始终为空.
    Index index;
    // Polaris 的已完整安装位置, 新准备状态初始为指定版本, 不在本机重新编号.
    std::uint64_t version{};
    // 有效 Key 和载荷字节的合计, 与 entries/index 在无异常提交阶段一起更新.
    std::size_t bytes{};
};

Almanac::Usage Almanac::usage() const {
    const std::lock_guard lock(*gate_); // 同一边界捕获计费和完整安装位置.
    return Usage{state_->bytes, history_bytes_, ready_ ? std::optional(state_->version) : std::nullopt};
}

std::size_t Almanac::Change::bytes() const noexcept {
    return sizeof(Change) + (key ? key->size() : 0) + (value ? value->size() : 0);
}

Almanac::View::Fence::~Fence() {
    // 此锁只同步遍历完成, 不覆盖遍历本身或用户回调; 在每个读区退出路径执行.
    const std::lock_guard lock(*gate);
}

Almanac::View::View(Index::View root, std::uint64_t version, std::size_t bytes, std::shared_ptr<std::mutex> gate) : root_(std::move(root)), version_(version), size_(root_.size()), bytes_(bytes), gate_(std::move(gate)) {}

std::uint64_t Almanac::View::version() const noexcept {
    return version_;
}

std::size_t Almanac::View::size() const noexcept {
    return size_;
}

std::size_t Almanac::View::bytes() const noexcept {
    return bytes_;
}

Almanac::Draft::Draft(std::uint64_t version, Limits limits) : state_(std::make_unique<State>()), limits_(limits) {
    state_->version = version;
}

Almanac::Draft::~Draft() = default;

Almanac::Draft::Draft(Draft&&) noexcept = default;

Almanac::Draft& Almanac::Draft::operator=(Draft&&) noexcept = default;

std::expected<void, Almanac::Error> Almanac::Draft::set(std::string key, Buffer value) {

    if (!state_ || !Almanac::valid(key, &value)) {
        return std::unexpected(Error::input);
    }
    if (state_->version == 0) {
        return std::unexpected(Error::version);
    }
    if (state_->entries.contains(key)) {
        return std::unexpected(Error::duplicate);
    }

    // bytes 只含当前新增 Key/正文, 单记录已被限制, 加法不会接近 size_t 上界.
    const auto bytes = key.size() + value.size();
    if (state_->entries.size() >= limits_.records || bytes > limits_.bytes - state_->bytes) {
        return std::unexpected(Error::capacity);
    }

    // name/shared 接管输入, slot 只为新有效项取得一次; 之后的失败必须撤回临时 Map 条目.
    auto name = std::make_shared<const std::string>(std::move(key));
    auto shared = std::make_shared<const Buffer>(std::move(value));
    const auto slot = state_->index.next();
    // entry 是尚未发布的新增入口, inserted 应始终为 true, 重复输入已经在上方拒绝.
    const auto [entry, inserted] = state_->entries.try_emplace(*name, State::Entry{name, shared, slot});
    if (!inserted) {
        return std::unexpected(Error::duplicate);
    }
    try {
        state_->index.prepare(slot);
    } catch (...) {
        state_->entries.erase(entry);
        throw;
    }

    // 所有分配已经成功, 在唯一候选内同时完成内容索引与计费; 不访问当前活动 Scope.
    state_->index.set(slot, std::move(name), {std::move(shared), {}});
    state_->bytes += bytes;
    return {};
}

Almanac::Almanac() : Almanac(Limits{}) {}

Almanac::Almanac(Limits limits) : limits_(limits), state_(std::make_unique<State>()) {}

Almanac::~Almanac() = default;

Almanac::Draft Almanac::prepare(std::uint64_t version) const {
    return Draft(version, limits_);
}

std::expected<bool, Almanac::Error> Almanac::reset(Draft&& draft) {

    if (!draft.state_) {
        return std::unexpected(Error::input);
    }
    if (draft.state_->entries.size() > limits_.records || draft.state_->bytes > limits_.bytes) {
        return std::unexpected(Error::capacity);
    }

    // retired/history 先于 lock 构造. 根、Map 和旧历史的最后引用均在解锁后销毁.
    std::unique_ptr<State> retired;
    std::deque<Change> history;
    const std::lock_guard lock(*gate_);
    if (ready_ && draft.state_->version < state_->version) {
        return std::unexpected(Error::version);
    }
    if (ready_ && draft.state_->version == state_->version) {
        return false;
    }

    // 私有准备已经完成. 指针交换不分配, 同时发布完整数据、权威版本和新的历史边界.
    retired = std::move(state_);
    state_ = std::move(draft.state_);
    history.swap(history_);
    history_bytes_ = 0;
    ready_ = true;
    return true;
}

std::expected<bool, Almanac::Error> Almanac::apply(std::uint64_t version, std::string key, std::optional<Buffer> value, std::size_t retention, Change* committed) {

    if (!valid(key, value ? &*value : nullptr)) {
        return std::unexpected(Error::input);
    }
    if (version == 0) {
        return std::unexpected(Error::version);
    }

    // change 在进入提交保护前接管正文, key 暂由参数拥有; 已存在的 Key 稍后复用原共享字符串.
    // 不为每次覆盖/删除重新分配一个长 Key, 新 Key 的所有权准备仍在发布之前完成.
    Change change{{}, value ? std::make_shared<const Buffer>(std::move(*value)) : nullptr, version};
    // retired/previous 先于 lock 构造, 避免覆盖/淘汰时在业务锁内回收最后一份大正文.
    std::vector<Change> retired;
    Value previous;
    const std::lock_guard lock(*gate_);
    if (!ready_) {
        return std::unexpected(Error::unready);
    }
    if (version <= state_->version) {
        return false;
    }
    if (state_->version == std::numeric_limits<std::uint64_t>::max() || version != state_->version + 1) {
        return std::unexpected(Error::version);
    }

    // entry 定位唯一 Key, inserted 标记可撤回的新入口; 未找到的 Delete 不创建节点.
    auto entry = state_->entries.find(key);
    bool inserted = false;
    // bytes 先扣除旧 Key/值, 再检查新 Set. 单记录约束保证自身加法有界.
    auto bytes = state_->bytes;
    if (entry != state_->entries.end()) {
        change.key = entry->second.key;
        bytes -= change.key->size() + entry->second.value->size();
    } else {
        change.key = std::make_shared<const std::string>(std::move(key));
    }
    if (change.value) {
        // added 为此 Set 完整 Key/值的逻辑成本, 替换时旧成本已经扣除.
        const auto added = change.key->size() + change.value->size();
        if ((entry == state_->entries.end() && state_->entries.size() >= limits_.records) || added > limits_.bytes - bytes) {
            return std::unexpected(Error::capacity);
        }
        bytes += added;
    }

    // 预计算需要淘汰的历史前缀, 只为真实淘汰项准备锁外回收容器, 不每次复制全部历史.
    // cost 为新历史成本; retained/backlog 从包含新项的总量递减; evicted 统计含新项的待淘汰前缀.
    // 新项自身超预算或历史容量为零时, evicted 可达旧 size + 1; 此时 retained/backlog 同为零, 循环结束.
    const auto cost = change.bytes();
    if (cost > std::numeric_limits<std::size_t>::max() - history_bytes_ || history_.size() == std::numeric_limits<std::size_t>::max()) {
        return std::unexpected(Error::capacity);
    }
    auto retained = history_.size() + 1;
    auto backlog = history_bytes_ + cost;
    std::size_t evicted{};
    while (retained > limits_.history || backlog > std::min(limits_.backlog, retention)) {
        backlog -= evicted < history_.size() ? history_[evicted].bytes() : cost;
        ++evicted;
        --retained;
    }
    retired.reserve(evicted);

    // Map 和页面路径属于可失败准备, 历史入队是最后一项可失败操作.
    // 页面准备失败只留下内容相同的私有页, 新 Map 节点在 catch 中撤回.
    if (change.value && entry == state_->entries.end()) {
        // slot 是新占用槽号, result 同时返回稳定入口及是否新增, 保留异常撤回所需信息.
        const auto slot = state_->index.next();
        const auto result = state_->entries.try_emplace(*change.key, State::Entry{change.key, {}, slot});
        entry = result.first;
        inserted = result.second;
    }
    try {
        if (entry != state_->entries.end()) {
            state_->index.prepare(entry->second.slot);
        }
        // 先入队再淘汰, 包括即将被淘汰的新项; 若此处失败, 不发布状态或执行后面的 pop_front.
        history_.push_back(change);
    } catch (...) {
        if (inserted) {
            state_->entries.erase(entry);
        }
        throw;
    }

    // 此后只做不分配的发布. 同值 Set 和缺失 Delete 仍发布权威位置与完整历史.
    if (entry != state_->entries.end()) {
        previous = std::move(entry->second.value);
        if (change.value) {
            state_->index.set(entry->second.slot, inserted ? change.key : nullptr, {change.value, {}});
            entry->second.value = change.value;
        } else {
            // slot 在删除点查入口前保存, 不在 erase 后访问失效的迭代器或键视图.
            const auto slot = entry->second.slot;
            state_->entries.erase(entry);
            state_->index.erase(slot);
        }
    }
    state_->bytes = bytes;
    state_->version = version;
    history_bytes_ = backlog;
    if (committed) {
        *committed = change; // 只复制共享引用, 不分配或依赖新历史是否即将被淘汰.
    }
    // item 逐项移出已经计算好的前缀, 新项已入队, evicted 不超过当前 size; retired 不会重新分配.
    for (std::size_t item = 0; item < evicted; ++item) {
        retired.push_back(std::move(history_.front()));
        history_.pop_front();
    }
    return true;
}

std::expected<Almanac::View, Almanac::Error> Almanac::view() const {

    const std::lock_guard lock(*gate_);
    if (!ready_) {
        return std::unexpected(Error::unready);
    }
    return View(state_->index.capture(), state_->version, state_->bytes, gate_);
}

std::expected<Almanac::Point, Almanac::Error> Almanac::find(std::string_view key) const {

    if (!valid(key, nullptr)) {
        return std::unexpected(Error::input);
    }
    const std::lock_guard lock(*gate_);
    if (!ready_) {
        return std::unexpected(Error::unready);
    }
    // found 只在锁内借用可变 Map, 返回值复制载荷所有权而非正文.
    const auto found = state_->entries.find(key);
    return Point{state_->version, found == state_->entries.end() ? nullptr : found->second.value};
}

std::expected<Almanac::Replay, Almanac::Error> Almanac::replay(std::uint64_t since) const {

    const std::lock_guard lock(*gate_);
    if (!ready_) {
        return std::unexpected(Error::unready);
    }
    if (since > state_->version) {
        return std::unexpected(Error::version);
    }
    if (since == state_->version) {
        return Replay{state_->version, {}};
    }
    if (history_.empty() || since < history_.front().version - 1) {
        return std::unexpected(Error::history);
    }

    // begin 通过随机访问 deque 的有序版本二分定位, 只复制受历史预算限制的元数据和共享引用.
    const auto begin = std::ranges::upper_bound(history_, since, {}, &Change::version);
    return Replay{state_->version, {begin, history_.end()}};
}

bool Almanac::valid(std::string_view key, const Buffer* value) noexcept {
    return !key.empty() && key.size() <= 1024 && key.find('\0') == std::string_view::npos && (!value || value->size() <= 1024 * 1024);
}
} // namespace astra
