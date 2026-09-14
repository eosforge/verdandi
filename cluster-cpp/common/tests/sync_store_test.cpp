#include "check.hpp"
#include "sync_store.hpp"

#include <iostream>
#include <chrono>

using namespace verdandi::cluster;
using namespace std::chrono_literals;

void test_put_and_remove() {
    SyncStore store;
    CHECK(store.global_version() == 0);

    std::vector<uint8_t> payload1{1, 2, 3};
    store.put("key1", std::move(payload1));
    
    CHECK(store.global_version() == 1);

    auto snapshot1 = store.get_snapshot();
    CHECK(snapshot1->global_version == 1);
    CHECK(snapshot1->data.size() == 1);

    store.remove("key1");
    CHECK(store.global_version() == 2);

    auto snapshot2 = store.get_snapshot();
    CHECK(snapshot2->global_version == 2);
    CHECK(snapshot2->data.size() == 0);
}

void test_evict_expired() {
    SyncStore store;
    
    auto now = std::chrono::steady_clock::now();
    std::vector<uint8_t> payload1{1};
    std::vector<uint8_t> payload2{2};

    store.put("key1", std::move(payload1), now + 10s);
    store.put("key2", std::move(payload2), Clock::time_point::max());

    CHECK(store.global_version() == 2);
    
    store.evict_expired(now + 5s);
    CHECK(store.global_version() == 2);

    store.evict_expired(now + 15s);
    CHECK(store.global_version() == 3);

    auto snapshot = store.get_snapshot();
    CHECK(snapshot->global_version == 3);
    CHECK(snapshot->data.size() == 1);
    CHECK(snapshot->data.find("key2") != snapshot->data.end());
    CHECK(snapshot->data.find("key1") == snapshot->data.end());
}

void test_extract_since_and_tombstone_gc() {
    SyncStore store(2); 

    std::vector<uint8_t> payload;
    
    store.put("k1", payload); // v1
    store.put("k2", payload); // v2
    store.remove("k1");       // v3 (tombstone)

    auto res1 = store.extract_since(2);
    CHECK(!res1.require_snapshot);
    CHECK(res1.current_version == 3);
    CHECK(res1.deltas.size() == 1);
    CHECK(res1.deltas[0].deleted);

    auto res2 = store.extract_since(1);
    CHECK(!res2.require_snapshot);
    CHECK(res2.current_version == 3);
    CHECK(res2.deltas.size() == 2);

    auto res3 = store.extract_since(0);
    CHECK(res3.require_snapshot);

    auto res_future = store.extract_since(100);
    CHECK(res_future.require_snapshot);
}

int main() {
    try {
        test_put_and_remove();
        test_evict_expired();
        test_extract_since_and_tombstone_gc();
        std::cout << "All tests passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed: " << e.what() << '\n';
        return 1;
    }
}
