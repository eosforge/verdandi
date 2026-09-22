#include "check.hpp"
#include "edition.hpp"
#include <iostream>
#include <map>

namespace {
using astra::Almanac;
using astra::Edition;

// 将完整冻结批次还原为可比较结果, 本用例只接受最后一页带完成字段.
std::map<std::string, std::optional<std::string>> consume(Edition& edition, proto::comet::v1::Mode mode, std::size_t minimum = 1) {

    std::map<std::string, std::optional<std::string>> records; // 每批 Key 必须唯一, 不用覆盖掩盖重复.
    std::size_t pages{};
    while (!edition.complete()) {
        auto page = edition.next("star-test"); // 每页独立拥有编码, 不保留对 Edition 内部对象的可写引用.
        CHECK(page.mode() == mode && page.ByteSizeLong() <= 8 * 1024 * 1024);
        if (page.complete()) {
            CHECK(page.has_version() && page.version() == edition.version() && page.instance() == "star-test");
        } else {
            CHECK(!page.has_version() && page.instance().empty() && !page.changes().empty());
        }
        for (const auto& change : page.changes()) {
            CHECK(!records.contains(change.key()));
            if (change.action_case() == proto::comet::v1::AlmanacChange::kValue) {
                records.emplace(change.key(), change.value());
            } else {
                CHECK(mode == proto::comet::v1::MODE_APPLY && change.action_case() == proto::comet::v1::AlmanacChange::kErase);
                records.emplace(change.key(), std::nullopt);
            }
        }
        ++pages;
        CHECK(pages <= 1024);
    }
    CHECK(pages >= minimum);
    bool failed{}; // 不能反复构造同一完成帧而伪造继续推进.
    try {
        static_cast<void>(edition.next("star-test"));
    } catch (const std::logic_error&) {
        failed = true;
    }
    CHECK(failed);
    return records;
}

// 不存在范围、精确缺项和已存在零字节值保持明确区别, 空基线也必须 complete.
void empty() {

    Edition absent;
    CHECK(consume(absent, proto::comet::v1::MODE_RESET).empty() && absent.version() == 0);
    Edition missing("key", Almanac::Point{5, {}});
    CHECK(consume(missing, proto::comet::v1::MODE_RESET).empty() && missing.version() == 5);
    Edition zero("key", Almanac::Point{6, std::make_shared<const Almanac::Buffer>()});
    CHECK(consume(zero, proto::comet::v1::MODE_RESET).at("key") == std::optional<std::string>(""));
    Edition caught(Almanac::Replay{9, {}});
    CHECK(consume(caught, proto::comet::v1::MODE_APPLY).empty() && caught.version() == 9);
}

// 跨页发送期间当前状态可以变化, 原捕获根和目标版本不能混入后来字节; 大记录独占一页.
void snapshots() {

    Almanac book;
    auto draft = book.prepare(1); // 601 项跨条数和字节分页, 第一条 1 MiB 超过软目标但合法.
    CHECK(draft.set("large", Almanac::Buffer(1024 * 1024, 7)));
    for (unsigned index = 0; index < 600; ++index) {
        CHECK(draft.set(std::to_string(index), {1}));
    }
    CHECK(book.reset(std::move(draft)));
    Edition edition(*book.view());
    const auto retained = edition.bytes(); // 冻结账面字节不随新值或新 Key 变动.
    CHECK(book.apply(2, "large", std::nullopt));
    CHECK(book.apply(3, "0", Almanac::Buffer{3}));
    CHECK(book.apply(4, "later", Almanac::Buffer{4}));
    const auto records = consume(edition, proto::comet::v1::MODE_RESET, 4);
    CHECK(records.size() == 601 && records.at("large")->size() == 1024 * 1024);
    CHECK(records.at("0") == std::optional<std::string>(std::string(1, '\1')));
    CHECK(!records.contains("later") && edition.version() == 1 && edition.bytes() == retained);
}

// 同 Key 的多次 Set/Delete 在冻结区间内只发送最终 action, 之后的写入进入另一批.
void changes() {

    Almanac book;
    CHECK(book.reset(book.prepare(0)));
    CHECK(book.apply(1, "first", Almanac::Buffer{1}));
    CHECK(book.apply(2, "second", Almanac::Buffer{2}));
    CHECK(book.apply(3, "first", std::nullopt));
    CHECK(book.apply(4, "second", Almanac::Buffer{}));
    Edition full(*book.replay(0));
    Edition exact(*book.replay(0), "second");
    Edition unrelated(*book.replay(0), "unknown");
    CHECK(book.apply(5, "first", Almanac::Buffer{5}));
    const auto output = consume(full, proto::comet::v1::MODE_APPLY);
    CHECK(output.size() == 2 && !output.at("first") && output.at("second") == std::optional<std::string>(""));
    const auto point = consume(exact, proto::comet::v1::MODE_APPLY);
    CHECK(point.size() == 1 && point.at("second") == std::optional<std::string>(""));
    CHECK(consume(unrelated, proto::comet::v1::MODE_APPLY).empty());
    CHECK(full.version() == 4 && exact.version() == 4 && unrelated.version() == 4);
}
} // namespace

// 仅由显式测试命令运行, 不创建网络服务或读取部署底稿.
int main() {
    try {
        empty();
        snapshots();
        changes();
        std::cout << "PASS frozen Almanac stream editions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
