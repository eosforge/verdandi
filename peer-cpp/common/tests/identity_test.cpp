#include "fixture.hpp"

#include <iostream>
#include <stdexcept>
#include <yyjson.h>

using namespace verdandi::peer;
#define CHECK(condition)                                                                                                                                       \
    do {                                                                                                                                                       \
        if (!(condition))                                                                                                                                      \
            throw std::runtime_error("Identity check failed at line " + std::to_string(__LINE__));                                                             \
    } while (false)

std::string_view text(yyjson_val* object, const char* field) {
    auto* value = yyjson_obj_get(object, field);
    CHECK(yyjson_is_str(value));
    return {yyjson_get_str(value), yyjson_get_len(value)};
}

void vectors(const Identity& identity) {
    auto json = test::read(std::filesystem::path(VERDANDI_FIXTURES) / "admission-v4.json");
    std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> document(yyjson_read(json.data(), json.size(), 0), yyjson_doc_free);
    CHECK(document);
    auto* root = yyjson_doc_get_root(document.get());
    for (const auto* field : {"names", "addresses", "uuids"}) {
        auto iterator = yyjson_arr_iter_with(yyjson_obj_get(root, field));
        while (auto* item = yyjson_arr_iter_next(&iterator)) {
            const auto value = text(item, "value");
            const auto expected = yyjson_get_bool(yyjson_obj_get(item, "valid"));
            bool actual = false;
            if (std::string_view(field) == "names") {
                actual = valid_name(value);
            } else if (std::string_view(field) == "addresses") {
                auto endpoint = Endpoint::parse(value);
                actual = endpoint && endpoint->text() == value;
            } else {
                auto id = PeerId::parse(value);
                actual = id && valid_uuid(*id);
            }
            CHECK(actual == expected);
        }
    }
    CHECK(identity.principal("alpha", "127.0.0.1:7443").text() == "40ff2c81b912f4ba03daa166aa9ad70a81d80c2b1c33e85cf9e2c18e43a072dd");
    CHECK(identity.principal("alpha", "[2001:db8::1]:7443").text() == "50bd7de8c9248ba9c0484566d54d7f7795b4d66cb68efb4bea888b765b8551d2");
}

void signatures(const Identity& identity) {
    wire::RegistrationResponse::Member member;
    member.set_cluster_id("alpha");
    member.set_group("default");
    member.set_peer_id("00000001000040008000000000000000");
    member.set_principal(identity.principal("alpha", "127.0.0.1:7443").text());
    member.set_advertise("127.0.0.1:7443");
    member.set_epoch(1);
    member.set_role(wire::NODE_ROLE_STAR);
    auto payload = member.SerializeAsString();
    auto signature = test::sign(payload);
    CHECK(identity.verify(payload, signature));
    CHECK(!identity.verify(payload, test::sign(payload, "wrong-domain")));
    CHECK(!identity.verify(payload + "\x78\x01", signature));
    const auto extended = payload + "\x78\x01";
    CHECK(identity.verify(extended, test::sign(extended)));
    CHECK(!identity.verify(payload, signature.substr(1)));
    signature[0] ^= 1;
    CHECK(!identity.verify(payload, signature));
    CHECK(!identity.verify(std::string(1025, 'x'), std::string(64, 'x')));
    member.set_epoch(0);
    CHECK(!decode_member(member));
    member.set_epoch(1);
    member.set_advertise("127.0.0.1:07443");
    CHECK(!decode_member(member));
}

int main() {
    try {
        const auto endpoint = *Endpoint::parse("127.0.0.1:7443");
        auto identity = Identity::load(std::filesystem::path(VERDANDI_FIXTURES) / "peer-a", endpoint);
        CHECK(identity);
        CHECK(!Identity::load(std::filesystem::path(VERDANDI_FIXTURES) / "expired", endpoint));
        CHECK(!Identity::load(std::filesystem::path(VERDANDI_FIXTURES) / "peer-a", *Endpoint::parse("192.0.2.1:7443")));
        const auto first = Identity::new_id(), second = Identity::new_id();
        CHECK(first && second && *first != *second);
        vectors(**identity);
        signatures(**identity);
        std::cout << "PASS shared v4 vectors, raw-byte Ed25519, local certificate validation\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
