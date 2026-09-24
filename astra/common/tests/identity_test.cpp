#include "check.hpp"
#include "fixture.hpp"

#include <iostream>
#include <stdexcept>
#include <yyjson.h>

using namespace astra;

namespace {

// 公共监听只依赖证书与私钥, 不读取内部准入、账号或 CA; 所有文件限于本例临时目录.
void external() {

    // directory 使用原子排他创建, 析构只清理本例已经取得所有权的路径.
    struct Directory {
        std::filesystem::path path; // 当前进程独占的临时路径, 不覆盖已有目录.

        Directory() {
            for (unsigned attempt = 0; attempt < 32; ++attempt) {
                path = std::filesystem::temp_directory_path() / ("astra-public-tls-" + std::to_string(Steady::now().time_since_epoch().count()) + "-" + std::to_string(attempt));
                if (std::filesystem::create_directory(path)) {
                    return;
                }
            }
            throw std::runtime_error("Cannot create public TLS test directory");
        }

        ~Directory() {
            std::error_code ignored; // 清理不得覆盖原始测试异常, 正常路径仍显式校验文件删除.
            std::filesystem::remove_all(path, ignored);
        }
    } directory;

    const auto fixtures = std::filesystem::path(ASTRA_FIXTURES); // 仅公开测试材料.
    for (const auto* name : {"cert.pem", "key.pem"}) {
        std::filesystem::copy_file(fixtures / "star-a" / name, directory.path / name);
    }
    const auto credentials = Identity::external(directory.path); // 无 ca.pem、login.json 或 admission.pub 仍须成功.
    if (!credentials) {
        throw std::runtime_error(credentials.error().message);
    }

    // 有效叶子后跟截断的中间证书也必须拒绝, 不能只验证第一张后把坏链交给后台握手.
    {
        std::ofstream output(directory.path / "cert.pem", std::ios::app);
        output << "\n-----BEGIN CERTIFICATE-----\nbroken\n";
    }
    CHECK(!Identity::external(directory.path));
    std::filesystem::copy_file(fixtures / "star-a/cert.pem", directory.path / "cert.pem", std::filesystem::copy_options::overwrite_existing);

    // 证书/私钥不匹配、过期、缺失均必须拒绝, 不因公共端口无需客户端 CA 而放宽材料校验.
    std::filesystem::copy_file(fixtures / "star-b/key.pem", directory.path / "key.pem", std::filesystem::copy_options::overwrite_existing);
    CHECK(!Identity::external(directory.path));
    CHECK(!Identity::external(fixtures / "expired"));
    CHECK(std::filesystem::remove(directory.path / "key.pem"));
    CHECK(!Identity::external(directory.path));
    CHECK(std::filesystem::remove(directory.path / "cert.pem"));
    CHECK(std::filesystem::remove(directory.path));
}

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
    const auto digest = identity.principal("alpha", "127.0.0.1:7443");
    member.set_principal(digest.bytes.data(), digest.bytes.size());
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
    // 每个基础设施角色均须显式编解码往返; 未知数值不能默默变成 Star/Planet.
    for (const auto role : {Member::Role::star, Member::Role::planet, Member::Role::polaris, Member::Role::astrolabe}) {
        member.set_role(Identity::role(role));
        // decoded 拥有经边界校验的成员, 成功角色必须与输入完全一致.
        const auto decoded = decode_member(member);
        CHECK(decoded && decoded->role == role && Identity::encode(*decoded).SerializeAsString() == member.SerializeAsString());
    }
    member.set_role(static_cast<proto::orbit::v1::Role>(99));
    CHECK(!decode_member(member));
    CHECK(Identity::role(static_cast<Member::Role>(99)) == proto::orbit::v1::ROLE_UNSPECIFIED);
    member.set_role(proto::orbit::v1::ROLE_STAR);
    // bytes 不再由 Protobuf 拒绝畸形 UTF-8, 验签成功仍必须执行跨语言一致的身份形状校验.
    for (const auto& invalid : {std::string("\xff", 1), std::string("\xc0\x80", 2), std::string("\xed\xa0\x80", 3), std::string("\xf4\x90\x80\x80", 4), std::string("\xe4\xb8", 2)}) {
        member.set_id(invalid);
        value = member.SerializeAsString();
        CHECK(!Member::valid_id(invalid));
        CHECK(!identity.verify(as_bytes(value), as_bytes(test::sign(value))));
    }
    // embedded 包含 NUL, 是合法 UTF-8 身份字节, 不按 C 字符串截断也不新增限制.
    const std::string embedded("a\0b", 3);
    member.set_id(embedded);
    CHECK(decode_member(member) && decode_member(member)->id == embedded);
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
        external();
        vectors(**identity);
        signatures(**identity);
        std::cout << "PASS shared v1 vectors, raw-byte Ed25519, local certificate validation\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
