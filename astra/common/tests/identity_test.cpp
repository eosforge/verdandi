// 功能: 验证本地身份材料, 准入验签与 TLS 凭证构造的成功和失败边界.
#include "check.hpp"
#include "fixture.hpp"

#include <iostream>
#include <stdexcept>
#include <yyjson.h>

using namespace astra;

std::string_view text(yyjson_val* object, const char* field) {
    auto* value = yyjson_obj_get(object, field);
    CHECK(yyjson_is_str(value));
    return {yyjson_get_str(value), yyjson_get_len(value)};
}

void vectors(const Identity& identity) {
    auto json = test::read(std::filesystem::path(ASTRA_FIXTURES) / "admission-v1.json");
    std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> document(yyjson_read(json.data(), json.size(), 0), yyjson_doc_free);
    CHECK(document);
    auto* root = yyjson_doc_get_root(document.get());
    for (const auto* field : {"names", "addresses", "ids"}) {
        auto iterator = yyjson_arr_iter_with(yyjson_obj_get(root, field));
        while (auto* item = yyjson_arr_iter_next(&iterator)) {
            const auto value = text(item, "value");
            const auto expected = yyjson_get_bool(yyjson_obj_get(item, "valid"));
            bool actual = false;
            if (std::string_view(field) == "names") {
                actual = Member::valid_name(value);
            } else if (std::string_view(field) == "addresses") {
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

void signatures(const Identity& identity) {
    proto::orbit::v1::Member member;
    member.set_galaxy("alpha");
    member.set_group("default");
    member.set_id("format-vNext/星体\"\nidentity");
    member.set_principal(identity.principal("alpha", "127.0.0.1:7443").text());
    member.set_advertise("127.0.0.1:7443");
    member.set_epoch(1);
    member.set_role(proto::orbit::v1::ROLE_STAR);
    auto value = member.SerializeAsString();
    auto signature = test::sign(value);
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

int main() {
    try {
        const auto endpoint = *Endpoint::parse("127.0.0.1:7443");
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
