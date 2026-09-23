#include "catalog_state.hpp"
#include "check.hpp"
#include <iostream>
#include <new>

namespace {
using State = astra::Catalog::State;
using Scene = State::Projection;
using Content = State::Content;
using Source = State::Source;

// 名称继续使用实际来源类型, 这里只探测公开内容能否满足无异常提交契约.
template <typename Item>
concept Accepted = requires { typename astra::Scene<Item, Source::Name>; };

static_assert(Accepted<Content> && !Accepted<std::string>); // 普通字符串的复制构造可能抛错, 应在类型边界直接拒绝.

// 正文计费复用实际内容类型, 无网络或对时依赖.
std::size_t measure(const Content& record) noexcept {
    return record.value->size();
}

// 本测试的所有名称共享同一地址, 测试旧名字寿命独立于来源实例.
Source::Tree::Key name(std::string key) {
    return std::make_shared<const Source::Name>(std::make_shared<const astra::Scope>("route", "main"), std::move(key));
}

// 公开记录只有版本及不透明正文, 不偷偷承载 TTL 或来源位置.
Content record(std::uint64_t version) {
    return {version, std::make_shared<const astra::Catalog::Buffer>(16, static_cast<std::uint8_t>(version))};
}

// 多目标投影只用于原子来源替换, 失败/放弃不发布根、游标、删除或目录容量.
void atomic() {

    const auto gate = std::make_shared<std::shared_mutex>();
    Scene scene(gate, measure, {.records = 2});
    const auto now = std::chrono::steady_clock::now();
    const auto one = name("one"), two = name("two"), three = name("three");
    std::optional<Scene::View> frozen;
    {
        Scene::Retired retired;
        const std::lock_guard lock(*gate);
        auto edit = scene.prepare(one, record(1), now);
        CHECK(edit);
        retired = edit->commit();
        frozen.emplace(scene.capture());
    }
    {
        const std::lock_guard lock(*gate);
        {
            auto batch = scene.prepare(4096);
            CHECK(batch.set(one, {}, now));
            CHECK(batch.set(two, record(2), now));
            CHECK(batch.set(three, record(3), now)); // 复用被删除槽位, 仍不得修改旧根.
        }
        CHECK(scene.capture().version() == 1 && scene.capture().size() == 1);
        CHECK(scene.find("one").record && !scene.find("two").record && !scene.find("three").record);
        CHECK(scene.replay(0)->size() == 1);
    }
    std::optional<Scene::Batch::Retired> retired; // 在真正提交后的 gate 之外回收旧树/历史.
    {
        const std::lock_guard lock(*gate);
        auto batch = scene.prepare(4096);
        CHECK(batch.find("one").record && !batch.find("two").record);
        CHECK(batch.set(one, {}, now));
        CHECK(batch.set(two, record(2), now));
        CHECK(!batch.find("two").record); // 查旧投影不能通过新目录误读被复用槽的旧 Key.
        CHECK(batch.set(three, record(3), now));
        retired.emplace(batch.commit());
        CHECK(scene.capture().version() == 4 && scene.capture().size() == 2);
        CHECK(!scene.find("one").record && scene.find("two").record->version == 2 && scene.find("three").record->version == 3);
        CHECK(retired->events.size() == 3 && retired->events[0].version == 2 && retired->events[2].version == 4);
        CHECK(scene.replay(1)->size() == 3);
    }
    frozen->each([](const auto& key, const Content& content) { CHECK(key == "one" && content.version == 1); });
}

// 重复目标、容量及历史零预算分别覆盖, 都不依赖断言在失败后自动补偿已发布部分.
void limits() {

    const auto gate = std::make_shared<std::shared_mutex>();
    Scene scene(gate, measure, {.records = 1, .history = 0});
    const auto now = std::chrono::steady_clock::now();
    const auto one = name("one"), two = name("two");
    {
        const std::lock_guard lock(*gate);
        {
            auto batch = scene.prepare(0);
            CHECK(batch.set(one, record(1), now));
            CHECK(!batch.set(one, record(2), now));
        }
        CHECK(scene.capture().version() == 0 && scene.capture().size() == 0);
        {
            auto batch = scene.prepare(0);
            CHECK(batch.set(one, record(1), now));
            CHECK(!batch.set(two, record(2), now));
        }
        CHECK(scene.capture().version() == 0 && scene.capture().size() == 0);
    }
    std::optional<Scene::Batch::Retired> retired;
    {
        const std::lock_guard lock(*gate);
        auto batch = scene.prepare(0);
        CHECK(batch.set(one, record(5), now));
        retired.emplace(batch.commit());
        CHECK(scene.capture().version() == 1 && scene.history() == 0);
        CHECK(scene.replay(0).error() == Scene::Error::history);
        CHECK(retired->events.size() == 1 && retired->events[0].record->version == 5); // 通知不从已裁剪历史恢复.
    }
}

// 超龄历史未裁剪时仍可恢复, 单项和内部批次写入在同一时间边界淘汰, 不遗漏删除事件.
void history(bool batch) {

    const auto gate = std::make_shared<std::shared_mutex>(); // 所有准备与读取共用同一域锁.
    Scene scene(gate, measure, {});                          // 默认历史保留时间为 10 分钟, 条数和字节足够本用例.
    const auto now = std::chrono::steady_clock::now();       // 当前维护时间, 不通过休眠等待历史变老.
    const auto stored = now - std::chrono::hours(1);         // 初始事件已超龄, 写入间隔为零, 尚无后续写入触发淘汰.
    const auto change = [&](std::uint64_t version, std::chrono::steady_clock::time_point stamp) {
        // version=0 表示删除, 其余值为公开内容版本; stamp 只控制历史裁剪, 不引入业务 TTL.
        Scene::Retired retired;                        // 单项通知和旧内容在解锁后释放.
        std::optional<Scene::Batch::Retired> replaced; // 批次交换的旧页和历史同样在锁外释放.
        const std::lock_guard lock(*gate);
        const auto value = version ? std::optional(record(version)) : std::nullopt; // 删除仍产生独立连续游标.
        if (batch) {
            auto edit = scene.prepare(4096); // 本批历史字节预算覆盖两条测试事件.
            CHECK(edit.set(name("key"), value, stamp));
            replaced.emplace(edit.commit());
        } else {
            auto edit = scene.prepare(name("key"), value, stamp); // 普通单目标热路径.
            CHECK(edit);
            retired = edit->commit();
        }
    };
    change(1, stored);
    change(0, stored);
    {
        const std::lock_guard lock(*gate);
        const auto replay = scene.replay(0); // 虽然已过一小时, 仍返回创建和删除的完整后缀.
        CHECK(replay && replay->size() == 2 && replay->front().record && !replay->back().record);
        CHECK(replay->front().version == 1 && replay->back().version == 2 && replay->front().stored == stored);
        CHECK(scene.replay(2)->empty() && scene.replay(3).error() == Scene::Error::version);
        CHECK(scene.capture().size() == 0); // 重放只读取历史, 不能把被删除的内容写回当前投影.
    }

    change(3, now); // 新写入实际裁掉旧创建/删除, 不能跳过丢失版本拼接一个看似完整的后缀.
    {
        const std::lock_guard lock(*gate);
        CHECK(scene.replay(0).error() == Scene::Error::history && scene.replay(1).error() == Scene::Error::history);
        const auto replay = scene.replay(2); // 已持有旧删除边界的读者仍能承接新创建.
        CHECK(replay && replay->size() == 1 && replay->front().version == 3 && replay->front().record);
    }
    change(0, now + std::chrono::minutes(10)); // 恰好达到保留时间的下一次写入也必须裁剪.
    {
        const std::lock_guard lock(*gate);
        CHECK(scene.replay(2).error() == Scene::Error::history);
        const auto replay = scene.replay(3); // 新删除仍可用于清空接收方原有记录.
        CHECK(replay && replay->size() == 1 && replay->front().version == 4 && !replay->front().record);
    }
}

// 撤销全部外部所有权和历史后仍可查找/覆盖, 删除后原名称必须真正释放; key 覆盖短串及最长合法串.
void ownership(const std::string& key, Scene::Limits limits) {

    // gate 串行保护事务; limits 分别禁用历史条数、字节或保留时间; now 固定, 不依赖真实等待.
    const auto gate = std::make_shared<std::shared_mutex>();
    Scene scene(gate, measure, limits);
    const auto now = std::chrono::steady_clock::now();
    std::weak_ptr<const Source::Name> observed; // 只观察最初 Name 的寿命, 不替生产树延长所有权.
    {
        Scene::Retired retired; // 提交通知在解锁后释放, 不让它掩盖树缺失所有权的问题.
        const std::lock_guard lock(*gate);
        auto original = name(key); // 独立 Name, 不与调用方 key 的字符存储共用内存.
        observed = original;
        auto edit = scene.prepare(std::move(original), record(1), now);
        CHECK(edit && !original);
        retired = edit->commit();
        CHECK(scene.history() == 0);
        CHECK(scene.replay(0).error() == Scene::Error::history && scene.replay(1)->empty()); // 零预算确实不保留刚写入的历史.
    }
    CHECK(!observed.expired()); // 现在只依赖活动树, Event/调用方都已释放自己的名称引用.

    {
        Scene::Retired retired; // 覆盖事件退出后也不再替目录保活.
        const std::lock_guard lock(*gate);
        const auto before = scene.find(key); // 查到完整旧内容后才准备覆盖, 不对缺项解引用.
        CHECK(before.record && before.record->version == 1);
        auto edit = scene.prepare(name(key), record(2), now); // 等值但地址不同的输入应复用原名称.
        CHECK(edit);
        retired = edit->commit();
        const auto after = scene.find(key); // 公开点查必须已切到新内容, 外层历史仍为空.
        CHECK(scene.history() == 0 && after.record && after.record->version == 2);
    }
    CHECK(!observed.expired());

    {
        Scene::Retired retired; // 删除通知结束后, 名称不再有任何持有者.
        const std::lock_guard lock(*gate);
        auto edit = scene.prepare(name(key), {}, now);
        CHECK(edit);
        retired = edit->commit();
        CHECK(!scene.find(key).record && scene.capture().size() == 0 && scene.history() == 0);
    }
    CHECK(observed.expired()); // 排除通过泄漏 Name 来掩盖悬垂借用的错误实现.
}

// 调用方在成功 prepare 后抛错, 原/移后事务均析构; 无半项、名称泄漏或永久 editing 状态.
void rollback() {

    // batch 分别检查单目标 Edit 和内部 Batch, 每轮用新 Scene 隔离目录容量及游标.
    for (const bool batch : {false, true}) {
        const auto gate = std::make_shared<std::shared_mutex>();  // 外层域锁, 必须晚于事务释放.
        Scene scene(gate, measure, {.records = 1, .history = 0}); // 只允许一个 Key, 暂存目录未撤销时后续新增会失败.
        const auto now = std::chrono::steady_clock::now();        // 同轮所有操作使用同一单调采样.
        std::weak_ptr<const Source::Name> observed;               // 失败事务不能泄漏未发布名称.
        bool caught{};                                            // 确认发生的是显式调用方异常, 不接受静默正常返回.
        bool raised{};                                            // 只有准备/移动均成功才置位, 排除意外的提前分配失败.
        try {
            const std::lock_guard lock(*gate);
            auto original = name("pending"); // 准备后放弃全部外部强引用.
            observed = original;
            if (batch) {
                auto prepared = scene.prepare(0); // 零历史字节候选, 仍须能正常回滚临时目录.
                CHECK(prepared.set(std::move(original), record(1), now));
                [[maybe_unused]] auto active = std::move(prepared); // 回滚责任只在移后候选, 原对象析构不得重复撤销.
                raised = true;
                throw std::bad_alloc{};
            }
            auto prepared = scene.prepare(std::move(original), record(1), now);
            CHECK(prepared);
            [[maybe_unused]] auto active = std::move(*prepared); // 同样覆盖 expected 内移后空事务的析构.
            raised = true;
            throw std::bad_alloc{};
        } catch (const std::bad_alloc&) {
            caught = true;
        }
        CHECK(caught && raised && observed.expired());

        Scene::Retired retired; // 正常恢复提交的通知在域锁之外析构.
        const std::lock_guard lock(*gate);
        CHECK(scene.capture().version() == 0 && scene.capture().size() == 0);
        const auto replay = scene.replay(0); // 回滚后仍是合法空历史, 显式排除错误结果再读取容器.
        CHECK(!scene.find("pending").record && scene.history() == 0 && replay && replay->empty());
        auto edit = scene.prepare(name("next"), record(2), now); // 可正常新增才证明 editing 与容量均已复原.
        CHECK(edit);
        retired = edit->commit();
        const auto next = scene.find("next"); // 新事务已经发布, 同时确认内容与游标可正常继续增长.
        CHECK(scene.capture().version() == 1 && next.record && next.record->version == 2);
    }
}
} // namespace

// 仅覆盖投影原子准备/发布, 不把通过此组件用例等同于多 Star 复制已验收.
int main() {

    try {
        atomic();
        limits();
        history(false);
        history(true);
        // key 同时覆盖短串与堆分配长串, 三种零历史边界均不能成为活动名称的唯一持有者.
        for (const auto& key : {std::string("s"), std::string(1024, 'k')}) {
            ownership(key, {.history = 0});
            ownership(key, {.backlog = 0});
            ownership(key, {.retention = std::chrono::steady_clock::duration::zero()});
        }
        rollback();
        std::cout << "projection batch: ok\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
