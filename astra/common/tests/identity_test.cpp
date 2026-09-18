#include "check.hpp"
#include "fixture.hpp"

#include <iostream>
#include <stdexcept>
#include <yyjson.h>

using namespace astra;

namespace {

// 借用 object 中指定 field 的字符串, 类型不符直接断言, 返回视图不超过 JSON 文档寿命.
std::string_view text(yyjson_val* object, const char* field) {

    // value 为字段借用指针, 必须是字符串才能读取字节和长度.
    auto* value = yyjson_obj_get(object, field);
    CHECK(yyjson_is_str(value));
    return {yyjson_get_str(value), yyjson_get_len(value)};
}

// 使用各语言共用的公开向量验证名称,端点,ID 与部署摘要, identity 只供摘要计算.
void vectors(const Identity& identity) {

    // json 拥有公开向量文件内容, yyjson 文档在本函数内独立管理.
    auto json = test::read(std::filesystem::path(ASTRA_FIXTURES) / "admission-v1.json");
    std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> document(yyjson_read(json.data(), json.size(), 0), yyjson_doc_free);
    CHECK(document);
    // root 借用已成功解析的文档根, 数组迭代不修改原数据.
    auto* root = yyjson_doc_get_root(document.get());
    for (const auto* field : {"names", "addresses", "ids"}) {
        // iterator 借用当前类别数组, 每次只读取一个明确的 value/valid 用例.
        auto iterator = yyjson_arr_iter_with(yyjson_obj_get(root, field));
        while (auto* item = yyjson_arr_iter_next(&iterator)) {
            // value 借用当前测试向量文本, 不由被测解析器反向生成期望值.
            const auto value = text(item, "value");
            // expected 来自共享向量的有效性标志, 与 C++ 实际结果独立.
            const auto expected = yyjson_get_bool(yyjson_obj_get(item, "valid"));
            // actual 初始 false, 按名称,端点或 ID 路径获得实际判定.
            bool actual = false;
            if (std::string_view(field) == "names") {
                actual = Member::valid_name(value);
            } else if (std::string_view(field) == "addresses") {
                // endpoint 为地址解析结果, 还需回编码相同才能视为规范端点.
                auto endpoint = Endpoint::parse(value);
                actual = endpoint && endpoint->text() == value;
            } else {
                actual = Member::valid_id(value);
            }
            CHECK(actual == expected);
        }
    }
    CHECK(identity.principal("alpha", "127.0.0.1:7443").text() == "40ff2c81b912f4ba03daa166aa9ad70a81d80c2b1c33e85cf9e2c18e43a072dd");
    CHECK(identity.principal("alpha", "[2001:db8::1]:7443").text() == "50bd7de8c9248ba9c0484566d54d7f7795b4d66cb68efb4bea888b765b8551d2");
}

// 用 identity 验证真实 Ed25519 原始字节签名, 覆盖域隔离,未知字段,篡改和长度边界.
void signatures(const Identity& identity) {

    // member 使用合法但不透明的 Unicode/控制字符 ID, 验证不解析历史 HexID 格式.
    proto::orbit::v1::Member member;
    member.set_galaxy("alpha");
    member.set_group("default");
    member.set_id("format-vNext/星体\"\nidentity");
    member.set_principal(identity.principal("alpha", "127.0.0.1:7443").text());
    member.set_advertise("127.0.0.1:7443");
    member.set_epoch(1);
    member.set_role(proto::orbit::v1::ROLE_STAR);
    // value 拥有原始签名正文, 后续篡改必须重新签名才能通过.
    auto value = member.SerializeAsString();
    // signature 为公开夹具密钥生成的合法签名, 后续单字节翻转验证拒绝路径.
    auto signature = test::sign(value);
    // as_bytes 只把 sv 字节视图转换为只读 span, 不复制也不延长原字符串寿命.
    auto as_bytes = [](std::string_view sv) { return std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(sv.data()), sv.size()); };
    CHECK(identity.verify(as_bytes(value), as_bytes(signature)));
    CHECK(identity.verify(as_bytes(value), as_bytes(signature))->id == member.id());
    // 不同用途的签名不能作为准入凭证.
    const auto other_purpose_signature = test::sign(value, "proto.orbit.v1.other-purpose");
    CHECK(!identity.verify(as_bytes(value), as_bytes(other_purpose_signature)));
    // 相同密钥和正文也不能跨协议所有者使用; 准入属于 Orbit.
    const auto other_owner_signature = test::sign(value, "proto.astra.v1.admission");
    CHECK(!identity.verify(as_bytes(value), as_bytes(other_owner_signature)));
    CHECK(!identity.verify(as_bytes(value), as_bytes(test::sign(value, "wrong-domain"))));
    CHECK(!identity.verify(as_bytes(value + "\x78\x01"), as_bytes(signature)));
    // extended 在合法编码末尾添加未知字段, 旧签名失效, 对完整新正文签名后可接受.
    const auto extended = value + "\x78\x01";
    CHECK(identity.verify(as_bytes(extended), as_bytes(test::sign(extended))));
    CHECK(!identity.verify(as_bytes(value), as_bytes(signature.substr(1))));
    signature[0] ^= 1;
    CHECK(!identity.verify(as_bytes(value), as_bytes(signature)));
    CHECK(!identity.verify(as_bytes(std::string(1025, 'x')), as_bytes(std::string(64, 'x'))));
    member.set_epoch(0);
    CHECK(!decode_member(member));
    member.set_epoch(1);
    member.set_advertise("127.0.0.1:07443");
    CHECK(!decode_member(member));
}

} // namespace

// 进程入口, 捕获可报告异常并返回非零失败状态; 测试断言不受 NDEBUG 影响.
int main() {

    try {
        // endpoint 为证书已授权的回环端点, 作为过期和错误 SAN 情景的对照.
        const auto endpoint = *Endpoint::parse("127.0.0.1:7443");
        // identity 加载当前有效的公开证书与账号, 后续验签只使用其中的准入公钥.
        auto identity = Identity::load(std::filesystem::path(ASTRA_FIXTURES) / "star-a", endpoint);
        CHECK(identity);
        CHECK(!Identity::load(std::filesystem::path(ASTRA_FIXTURES) / "expired", endpoint));
        CHECK(!Identity::load(std::filesystem::path(ASTRA_FIXTURES) / "star-a", *Endpoint::parse("192.0.2.1:7443")));
        vectors(**identity);
        signatures(**identity);
        std::cout << "PASS shared v1 vectors, raw-byte Ed25519, local certificate validation\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
