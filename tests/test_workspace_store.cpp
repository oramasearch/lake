// lake#380: a workspace's liquid store in its workspace Zetta (docs/architecture/workspace-store.md).
//
// The backend, the encryption below liquid, and the owner run against FakeZettaObjects, which
// answers Zetta's object routes as zetta docs/data/object-store.md specifies them, including the
// refusal of an in-place rewrite. The move runs through a real lake server: a workspace's
// repositories are planned, copied (once interrupted), verified and switched, then served and
// written from the workspace store while the space store is left as it was, and two workspaces of
// one space stay apart. WorkspaceStoreZetta.* run the same backend against a real Zetta binary
// when LAKE_TEST_ZETTA_BINARY names one, and print the latency beside the disk backend's.

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <liquid/hash.hpp>
#include <liquid/repository.hpp>
#include <liquid/store.hpp>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

#include "audit_test_support.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/server.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/postgres_store.hpp"
#include "lake/control/workspace_store.hpp"
#include "lake/control/zetta_control.hpp"
#include "lake/search/index.hpp"
#include "latency_proxy.hpp"
#include "one_thread_io.hpp"
#include "postgres_fixture.hpp"
#include "temporary_directory.hpp"
#include "workspace_fixture.hpp"
#include "zetta_object_fixture.hpp"

extern char** environ;

namespace {

using nlohmann::json;
using lake::control::ObjectEndpoint;
using lake::control::ZettaObjectBackend;
using lake::control::ZettaObjectBackendOptions;
using lake::control::ZettaObjectClient;
using lake::control::ZettaStoreOwner;
using namespace std::chrono_literals;

constexpr std::string_view kek = "0f1e2d3c4b5a69788796a5b4c3d2e1f000112233445566778899aabbccddeeff";
constexpr std::string_view other_kek =
    "ffeeddccbbaa99887766554433221100f0e1d2c3b4a5968778695a4b3c2d1e0f";
constexpr std::string_view marker = "LAKE-380-PLAINTEXT-MARKER-6f1c2a9e-never-in-a-bucket";

std::vector<std::byte> random_bytes(std::size_t size, std::uint64_t seed) {
    std::mt19937_64 generator(seed);
    std::vector<std::byte> output(size);
    for (std::size_t index = 0; index < size; index += 8) {
        const std::uint64_t value = generator();
        for (std::size_t part = 0; part < 8 && index + part < size; ++part) {
            output[index + part] = static_cast<std::byte>((value >> (part * 8)) & 0xffU);
        }
    }
    return output;
}

std::vector<std::byte> with_marker(std::vector<std::byte> bytes, std::size_t at) {
    std::copy(reinterpret_cast<const std::byte*>(marker.data()),
              reinterpret_cast<const std::byte*>(marker.data() + marker.size()), bytes.begin() + at);
    return bytes;
}

std::vector<std::byte> bytes_of(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()),
            reinterpret_cast<const std::byte*>(text.data() + text.size())};
}

bool holds_marker(const std::map<std::string, std::string>& objects) {
    return std::ranges::any_of(objects, [](const auto& object) {
        return object.second.find(marker) != std::string::npos;
    });
}

// One workspace Zetta in the fake and a workspace id for it.
struct Rig {
    std::shared_ptr<lake::test::FakeZettaObjects> zetta = std::make_shared<lake::test::FakeZettaObjects>();
    std::string workspace = lake::control::new_uuid_v4();
    std::string endpoint = "http://" + workspace + ".test";

    Rig() {
        zetta->add_node(endpoint, "token-" + workspace);
    }

    std::shared_ptr<ZettaObjectClient> client() const {
        return std::make_shared<ZettaObjectClient>(zetta, ObjectEndpoint{endpoint, "token-" + workspace});
    }

    // `create`: the backend may create the data key in an empty bucket, as lake's copy does.
    std::shared_ptr<ZettaObjectBackend> backend(ZettaObjectBackendOptions options = {},
                                                bool create = true) const {
        options.workspace_id = workspace;
        options.create_key = create;
        if (options.instance_kek.empty()) {
            options.instance_kek = std::string(kek);
        }
        auto made = client();
        EXPECT_TRUE(liquid::runtime::block_on(made->create_bucket(0)));
        return std::make_shared<ZettaObjectBackend>(made, std::move(options));
    }
};

// Small blocks and windows so a few KiB cross every boundary the layout has.
ZettaObjectBackendOptions small_blocks() {
    ZettaObjectBackendOptions options;
    options.block_size = 1024;
    options.flush_bytes = 4096;
    options.window_bytes = 8192;
    options.cache_bytes = 64 * 1024;
    return options;
}

std::vector<std::byte> read(lake::control::ZettaObjectBackend& backend, const std::string& key,
                            std::uint64_t offset, std::uint64_t size) {
    auto bytes = liquid::runtime::block_on(backend.get_range(key, offset, size));
    EXPECT_TRUE(bytes) << bytes.error().message;
    return bytes ? *bytes : std::vector<std::byte>{};
}

std::uint64_t size_of(lake::control::ZettaObjectBackend& backend, const std::string& key) {
    auto listed = liquid::runtime::block_on(backend.list(key));
    EXPECT_TRUE(listed) << listed.error().message;
    for (const auto& entry : listed ? *listed : std::vector<liquid::store_backend_entry>{}) {
        if (entry.key == key) {
            return entry.size;
        }
    }
    return 0;
}

TEST(WorkspaceStoreBackend, RangesRoundTripAcrossBlocksTailsAndReopen) {
    Rig rig;
    auto backend = rig.backend(small_blocks());
    const auto whole = with_marker(random_bytes(3000, 1), 100);
    ASSERT_TRUE(liquid::runtime::block_on(backend->put("objects/whole", whole)));
    // A small put is held until the barrier (lake#527) and read back from memory meanwhile.
    EXPECT_FALSE(rig.zetta->objects(rig.endpoint).contains("objects/whole"));
    EXPECT_EQ(read(*backend, "objects/whole", 0, 3000), whole);
    ASSERT_TRUE(liquid::runtime::block_on(backend->sync()));
    EXPECT_TRUE(rig.zetta->objects(rig.endpoint).contains("objects/whole"));
    EXPECT_EQ(read(*backend, "objects/whole", 0, 3000), whole);
    EXPECT_EQ(read(*backend, "objects/whole", 1000, 1100),
              std::vector<std::byte>(whole.begin() + 1000, whole.begin() + 2100));
    EXPECT_EQ(size_of(*backend, "objects/whole"), 3000U);

    // An object created by appends, grown past several flushes, a tail at each barrier.
    std::vector<std::byte> grown;
    const auto append = [&](std::size_t size, std::uint64_t seed) {
        const auto part = random_bytes(size, seed);
        auto written = liquid::runtime::block_on(backend->put_range("packs/pack-1.pack", grown.size(), part));
        ASSERT_TRUE(written) << written.error().message;
        grown.insert(grown.end(), part.begin(), part.end());
    };
    // Created and barriered empty, so what follows is appended (an object created and filled in
    // one interval goes as one PUT, with no tail).
    ASSERT_TRUE(liquid::runtime::block_on(backend->put_range("packs/pack-1.pack", 0, {})));
    ASSERT_TRUE(liquid::runtime::block_on(backend->sync()));
    append(100, 2);
    append(5000, 3);
    // Bytes not yet barriered read back from the buffer.
    EXPECT_EQ(read(*backend, "packs/pack-1.pack", 0, grown.size()), grown);
    ASSERT_TRUE(liquid::runtime::block_on(backend->sync()));
    const auto stored = rig.zetta->objects(rig.endpoint);
    EXPECT_TRUE(stored.contains("packs/pack-1.pack"));
    EXPECT_TRUE(std::ranges::any_of(stored, [](const auto& object) {
        return object.first.starts_with("packs/pack-1.pack.lake-tail-");
    }));
    append(2500, 4);
    ASSERT_TRUE(liquid::runtime::block_on(backend->sync()));
    // The replaced tail is removed beside the next barrier's writes, once its replacement is
    // durable; then exactly one current tail is kept.
    ASSERT_TRUE(liquid::runtime::block_on(backend->sync()));
    const auto after = rig.zetta->objects(rig.endpoint);
    EXPECT_EQ(std::ranges::count_if(after, [](const auto& object) {
                  return object.first.starts_with("packs/pack-1.pack.lake-tail-");
              }),
              1);

    // A new backend learns the layout from the bucket alone.
    auto reopened = rig.backend(small_blocks());
    EXPECT_EQ(size_of(*reopened, "packs/pack-1.pack"), grown.size());
    EXPECT_EQ(read(*reopened, "packs/pack-1.pack", 0, grown.size()), grown);
    EXPECT_EQ(read(*reopened, "packs/pack-1.pack", 1020, 10),
              std::vector<std::byte>(grown.begin() + 1020, grown.begin() + 1030));
    // and goes on appending from the stored tail.
    backend = reopened;
    append(700, 5);
    ASSERT_TRUE(liquid::runtime::block_on(backend->sync()));
    auto third = rig.backend(small_blocks());
    EXPECT_EQ(read(*third, "packs/pack-1.pack", 0, grown.size()), grown);

    // A put followed by appends in the same interval (an index run).
    std::vector<std::byte> run = random_bytes(40, 6);
    ASSERT_TRUE(liquid::runtime::block_on(third->put("index/run-1.idx", run)));
    const auto rest = random_bytes(2100, 7);
    ASSERT_TRUE(liquid::runtime::block_on(third->put_range("index/run-1.idx", 40, rest)));
    run.insert(run.end(), rest.begin(), rest.end());
    ASSERT_TRUE(liquid::runtime::block_on(third->sync()));
    auto fourth = rig.backend(small_blocks());
    EXPECT_EQ(read(*fourth, "index/run-1.idx", 0, run.size()), run);

    // Ranges outside an object, missing objects, the conditional create, remove.
    const auto outside = liquid::runtime::block_on(fourth->get_range("objects/whole", 2990, 20));
    ASSERT_FALSE(outside);
    EXPECT_EQ(outside.error().code, liquid::errc::corrupt);
    const auto missing = liquid::runtime::block_on(fourth->get_range("objects/none", 0, 1));
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, liquid::errc::not_found);
    const auto again = liquid::runtime::block_on(fourth->put_if_absent("objects/whole", bytes_of("x")));
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error().code, liquid::errc::already_exists);
    ASSERT_TRUE(liquid::runtime::block_on(fourth->put_if_absent("objects/fresh", bytes_of("fresh"))));
    EXPECT_EQ(read(*fourth, "objects/fresh", 0, 5), bytes_of("fresh"));
    ASSERT_TRUE(liquid::runtime::block_on(fourth->remove("packs/pack-1.pack")));
    // The removal is sent at the barrier, and the object's tails beside the barrier after it.
    ASSERT_TRUE(liquid::runtime::block_on(fourth->sync()));
    ASSERT_TRUE(liquid::runtime::block_on(fourth->sync()));
    const auto removed = rig.zetta->objects(rig.endpoint);
    EXPECT_FALSE(std::ranges::any_of(removed, [](const auto& object) {
        return object.first.starts_with("packs/pack-1.pack");
    }));
    const auto gone = liquid::runtime::block_on(fourth->remove("packs/pack-1.pack"));
    ASSERT_FALSE(gone);
    EXPECT_EQ(gone.error().code, liquid::errc::not_found);

    // Reserved keys stay out of liquid's reach, and no plaintext reached the bucket.
    EXPECT_FALSE(liquid::runtime::block_on(fourth->put("keys/workspace-v1.wrapped", bytes_of("x"))));
    EXPECT_FALSE(holds_marker(rig.zetta->objects(rig.endpoint)));
    const auto listed = liquid::runtime::block_on(fourth->list(""));
    ASSERT_TRUE(listed);
    for (const auto& entry : *listed) {
        EXPECT_FALSE(entry.key.starts_with("keys/")) << entry.key;
        EXPECT_EQ(entry.key.find(".lake-tail-"), std::string::npos) << entry.key;
    }
}

TEST(WorkspaceStoreBackend, ARewriteWithOtherBytesIsRefused) {
    Rig rig;
    auto backend = rig.backend(small_blocks());
    const auto header = random_bytes(64, 11);
    ASSERT_TRUE(liquid::runtime::block_on(backend->put("index/run-2.idx", header)));
    ASSERT_TRUE(liquid::runtime::block_on(backend->put_range("index/run-2.idx", 64, random_bytes(3000, 12))));
    ASSERT_TRUE(liquid::runtime::block_on(backend->sync()));
    // A retried range with the bytes already there changes nothing.
    EXPECT_TRUE(liquid::runtime::block_on(backend->put_range("index/run-2.idx", 0, header)));
    // A rewrite of written bytes with other bytes is refused, plainly.
    const auto rewritten =
        liquid::runtime::block_on(backend->put_range("index/run-2.idx", 0, random_bytes(64, 13)));
    ASSERT_FALSE(rewritten);
    EXPECT_EQ(rewritten.error().code, liquid::errc::conflict);
    EXPECT_NE(rewritten.error().message.find("only grow"), std::string::npos)
        << rewritten.error().message;
    // So is an append that skips past the end.
    const auto skipped =
        liquid::runtime::block_on(backend->put_range("index/run-2.idx", 9000, random_bytes(4, 14)));
    ASSERT_FALSE(skipped);
    EXPECT_EQ(skipped.error().code, liquid::errc::conflict);
}

TEST(WorkspaceStoreBackend, TamperingMovedBlocksAndAnotherWorkspacesKeyAreRefused) {
    Rig rig;
    auto backend = rig.backend(small_blocks());
    const auto contents = random_bytes(4000, 21);
    ASSERT_TRUE(liquid::runtime::block_on(backend->put("objects/a", contents)));
    ASSERT_TRUE(liquid::runtime::block_on(backend->put("objects/b", contents)));
    ASSERT_TRUE(liquid::runtime::block_on(backend->sync()));
    const auto stored = rig.zetta->objects(rig.endpoint);

    // One flipped bit in a block.
    std::string flipped = stored.at("objects/a");
    flipped[48 + 12 + 5] = static_cast<char>(flipped[48 + 12 + 5] ^ 0x01);
    rig.zetta->set_object(rig.endpoint, "objects/a", flipped);
    auto reader = rig.backend(small_blocks());
    const auto tampered = liquid::runtime::block_on(reader->get_range("objects/a", 0, 10));
    ASSERT_FALSE(tampered);
    EXPECT_EQ(tampered.error().code, liquid::errc::corrupt);

    // Two blocks swapped within an object, and an object's bytes under another key.
    const std::size_t block_ct = 1024 + lake::control::WorkspaceStoreCipher::overhead;
    std::string swapped = stored.at("objects/b");
    std::swap_ranges(swapped.begin() + 48, swapped.begin() + 48 + block_ct,
                     swapped.begin() + 48 + block_ct);
    rig.zetta->set_object(rig.endpoint, "objects/b", swapped);
    rig.zetta->set_object(rig.endpoint, "objects/c", stored.at("objects/a"));
    auto second = rig.backend(small_blocks());
    EXPECT_FALSE(liquid::runtime::block_on(second->get_range("objects/b", 0, 10)));
    const auto moved = liquid::runtime::block_on(second->get_range("objects/c", 0, 10));
    ASSERT_FALSE(moved);
    EXPECT_EQ(moved.error().code, liquid::errc::corrupt);

    // Another workspace's bucket holding these bytes, under its own key and under this one.
    Rig other;
    auto foreign = other.backend(small_blocks());
    ASSERT_TRUE(liquid::runtime::block_on(foreign->put("objects/own", bytes_of("own"))));
    ASSERT_TRUE(liquid::runtime::block_on(foreign->sync()));
    other.zetta->set_object(other.endpoint, "objects/a", stored.at("objects/a"));
    auto other_reader = other.backend(small_blocks());
    const auto copied = liquid::runtime::block_on(other_reader->get_range("objects/a", 0, 10));
    ASSERT_FALSE(copied);
    EXPECT_EQ(copied.error().code, liquid::errc::corrupt);
    other.zetta->set_object(other.endpoint, std::string(lake::control::workspace_store_key_object),
                            stored.at(std::string(lake::control::workspace_store_key_object)));
    auto with_foreign_key = other.backend(small_blocks());
    const auto unwrapped = liquid::runtime::block_on(with_foreign_key->get_range("objects/a", 0, 10));
    ASSERT_FALSE(unwrapped);
    EXPECT_EQ(unwrapped.error().code, liquid::errc::unauthorized);

    // The right bucket under another lake's KEK.
    ZettaObjectBackendOptions wrong = small_blocks();
    wrong.instance_kek = std::string(other_kek);
    auto wrong_kek = rig.backend(wrong);
    const auto refused = liquid::runtime::block_on(wrong_kek->get_range("objects/b", 0, 10));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::unauthorized);
    // A KEK that is not one at all.
    ZettaObjectBackendOptions none = small_blocks();
    none.instance_kek = "not-hex";
    auto no_kek = rig.backend(none);
    const auto unconfigured = liquid::runtime::block_on(no_kek->get_range("objects/b", 0, 10));
    ASSERT_FALSE(unconfigured);
    EXPECT_EQ(unconfigured.error().code, liquid::errc::not_configured);
}

liquid::store_options owned(const Rig& rig, std::shared_ptr<ZettaStoreOwner>& owner,
                            std::chrono::milliseconds ttl = 30s,
                            std::chrono::milliseconds wait = 0ms) {
    auto client = rig.client();
    EXPECT_TRUE(liquid::runtime::block_on(client->create_bucket(0)));
    owner = std::make_shared<ZettaStoreOwner>(client, ttl);
    liquid::store_options options;
    options.backend = std::make_shared<ZettaObjectBackend>(
        client, ZettaObjectBackendOptions{.workspace_id = rig.workspace,
                                          .instance_kek = std::string(kek),
                                          .create_key = true});
    options.owner = owner;
    options.owner_wait = wait;
    return options;
}

TEST(WorkspaceStoreBackend, ALiquidStoreHoldsOnlyCiphertextAndReopens) {
    Rig rig;
    const auto root = lake::test::fixture_path("workspace-store-liquid-" + rig.workspace);
    std::shared_ptr<ZettaStoreOwner> owner;
    std::vector<std::pair<liquid::digest, std::vector<std::byte>>> written;
    {
        auto store = liquid::runtime::block_on(liquid::store::open(root, owned(rig, owner)));
        ASSERT_TRUE(store) << store.error().message;
        for (std::uint64_t seed = 0; seed < 6; ++seed) {
            auto contents = with_marker(random_bytes(seed == 0 ? 3 * 1024 * 1024 : 20000 + seed * 999, 30 + seed), 64);
            auto id = liquid::runtime::block_on(store->put(contents));
            ASSERT_TRUE(id) << id.error().message;
            written.emplace_back(*id, std::move(contents));
        }
        ASSERT_TRUE(liquid::runtime::block_on(store->close()));
    }
    EXPECT_TRUE(rig.zetta->lease(rig.endpoint).holder.empty()) << "close releases the lease";
    EXPECT_FALSE(holds_marker(rig.zetta->objects(rig.endpoint)));
    EXPECT_FALSE(std::ranges::any_of(rig.zetta->bodies(), [](const std::string& body) {
        return body.find(marker) != std::string::npos;
    })) << "no request carried plaintext";
    auto store = liquid::runtime::block_on(liquid::store::open(root, owned(rig, owner)));
    ASSERT_TRUE(store) << store.error().message;
    for (const auto& [id, contents] : written) {
        auto read_back = liquid::runtime::block_on(store->get(id));
        ASSERT_TRUE(read_back) << read_back.error().message;
        EXPECT_EQ(*read_back, contents);
    }
    auto collected = liquid::runtime::block_on(store->collect({.dry_run = true}));
    ASSERT_TRUE(collected) << collected.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(store->close()));
    EXPECT_FALSE(std::filesystem::exists(root)) << "a workspace store writes nothing locally";
}

// Zetta's objects only grow, and liquid writes each index run once, in order (liquid#332), so a
// store that takes enough writes to merge its index runs works on the workspace store.
TEST(WorkspaceStoreBackend, AStoreThatMergesIndexRunsWritesThemOnce) {
    Rig rig;
    const auto root = lake::test::fixture_path("workspace-store-merge-" + rig.workspace);
    std::shared_ptr<ZettaStoreOwner> owner;
    std::vector<std::pair<liquid::digest, std::vector<std::byte>>> acknowledged;
    {
        auto store = liquid::runtime::block_on(liquid::store::open(root, owned(rig, owner)));
        ASSERT_TRUE(store) << store.error().message;
        for (std::uint64_t seed = 0; seed < 24; ++seed) {
            auto contents = random_bytes(9000 + seed * 13, 400 + seed);
            auto id = liquid::runtime::block_on(store->put(contents));
            ASSERT_TRUE(id) << id.error().message;
            acknowledged.emplace_back(*id, std::move(contents));
        }
        ASSERT_TRUE(liquid::runtime::block_on(store->close()));
    }
    auto store = liquid::runtime::block_on(liquid::store::open(root, owned(rig, owner)));
    ASSERT_TRUE(store) << store.error().message;
    for (const auto& [id, contents] : acknowledged) {
        auto read_back = liquid::runtime::block_on(store->get(id));
        ASSERT_TRUE(read_back) << read_back.error().message;
        EXPECT_EQ(*read_back, contents);
    }
    ASSERT_TRUE(liquid::runtime::block_on(store->close()));
}

TEST(WorkspaceStoreOwner, ASuccessorWaitsForTheHolderAndTakesOverWithAHigherFence) {
    Rig rig;
    const auto root = lake::test::fixture_path("workspace-store-handoff-" + rig.workspace);
    std::shared_ptr<ZettaStoreOwner> first_owner;
    auto first = liquid::runtime::block_on(liquid::store::open(root, owned(rig, first_owner)));
    ASSERT_TRUE(first) << first.error().message;
    const auto first_fence = first_owner->fence();
    auto acknowledged = liquid::runtime::block_on(first->put(random_bytes(50000, 41)));
    ASSERT_TRUE(acknowledged);

    std::shared_ptr<ZettaStoreOwner> refused_owner;
    auto refused = liquid::runtime::block_on(liquid::store::open(root, owned(rig, refused_owner)));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::locked);
    EXPECT_NE(refused.error().message.find("owned by"), std::string::npos) << refused.error().message;

    std::shared_ptr<ZettaStoreOwner> second_owner;
    std::optional<liquid::result<liquid::store>> second;
    std::thread waiter([&] {
        second.emplace(liquid::runtime::block_on(
            liquid::store::open(root, owned(rig, second_owner, 30s, 10s))));
    });
    std::this_thread::sleep_for(300ms);
    EXPECT_FALSE(second.has_value()) << "the successor waits while the holder runs";
    ASSERT_TRUE(liquid::runtime::block_on(first->close()));
    waiter.join();
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(*second) << (*second).error().message;
    EXPECT_GT(second_owner->fence(), first_fence);
    auto read_back = liquid::runtime::block_on((*second)->get(*acknowledged));
    EXPECT_TRUE(read_back) << read_back.error().message;
    ASSERT_TRUE(liquid::runtime::block_on((*second)->close()));
}

TEST(WorkspaceStoreOwner, ALostLeaseRefusesWritesAndTheSuccessorKeepsEveryAcknowledgedObject) {
    Rig rig;
    const auto root = lake::test::fixture_path("workspace-store-lost-" + rig.workspace);
    std::shared_ptr<ZettaStoreOwner> stale_owner;
    auto stale = liquid::runtime::block_on(liquid::store::open(root, owned(rig, stale_owner, 900ms)));
    ASSERT_TRUE(stale) << stale.error().message;
    std::vector<std::pair<liquid::digest, std::vector<std::byte>>> acknowledged;
    for (std::uint64_t seed = 0; seed < 3; ++seed) {
        auto contents = random_bytes(70000, 50 + seed);
        auto id = liquid::runtime::block_on(stale->put(contents));
        ASSERT_TRUE(id);
        acknowledged.emplace_back(*id, std::move(contents));
    }
    // A successor takes the lease; the holder's renewal is refused and it learns it lost.
    rig.zetta->take_over(rig.endpoint, "successor");
    for (int waited = 0; waited < 40 && !stale_owner->lost(); ++waited) {
        std::this_thread::sleep_for(100ms);
    }
    EXPECT_TRUE(stale_owner->lost());
    const auto refused = liquid::runtime::block_on(stale->put(random_bytes(5000, 60)));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::locked);
    rig.zetta->release(rig.endpoint);
    static_cast<void>(liquid::runtime::block_on(stale->close()));

    std::shared_ptr<ZettaStoreOwner> successor_owner;
    auto successor = liquid::runtime::block_on(liquid::store::open(root, owned(rig, successor_owner)));
    ASSERT_TRUE(successor) << successor.error().message;
    for (const auto& [id, contents] : acknowledged) {
        auto read_back = liquid::runtime::block_on(successor->get(id));
        ASSERT_TRUE(read_back) << read_back.error().message;
        EXPECT_EQ(*read_back, contents);
    }

    // A write the remote refuses as fenced revokes the grant at once, before any renewal.
    rig.zetta->take_over(rig.endpoint, "third");
    const auto fenced = liquid::runtime::block_on(successor->put(random_bytes(9000, 61)));
    ASSERT_FALSE(fenced);
    EXPECT_EQ(fenced.error().code, liquid::errc::locked);
    EXPECT_TRUE(successor_owner->lost());
    static_cast<void>(liquid::runtime::block_on(successor->close()));
}

TEST(WorkspaceStoreBackend, ATransientFailureIsRetriedAndAPersistentOneIsUnavailable) {
    Rig rig;
    auto backend = rig.backend(small_blocks());
    ASSERT_TRUE(liquid::runtime::block_on(backend->put("objects/x", random_bytes(10, 70))));
    ASSERT_TRUE(liquid::runtime::block_on(backend->sync()));
    // A small put is sent by the barrier (lake#527), which retries it.
    rig.zetta->drop_next(2);
    EXPECT_TRUE(liquid::runtime::block_on(backend->put("objects/y", random_bytes(10, 71))));
    EXPECT_TRUE(liquid::runtime::block_on(backend->sync()));
    rig.zetta->drop_next(10);
    ASSERT_TRUE(liquid::runtime::block_on(backend->put("objects/z", random_bytes(10, 72))));
    const auto failed = liquid::runtime::block_on(backend->sync());
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code, liquid::errc::unavailable);
}

// A write through lake succeeds.
bool written(const std::pair<unsigned, json>& answer) {
    EXPECT_EQ(answer.first, 200U) << answer.second.dump();
    return answer.first == 200U;
}

// A push through lake succeeds.
bool pushed(const std::string& error) {
    EXPECT_TRUE(error.empty()) << error;
    return error.empty();
}

std::optional<lake::control::Repository> MoveRig_create(unsigned status, const json& body) {
    if (status == 200U) {
        lake::control::Repository output;
        output.id = body["value"].value("id", std::string());
        return output;
    }
    ADD_FAILURE() << status << " " << body.dump();
    return std::nullopt;
}

// ---- the move, through a lake server ---------------------------------------------------------

std::string hash_key(const std::filesystem::path& identifier) {
    const std::string text = identifier.generic_string();
    const auto id = liquid::hash(liquid::algorithm::blake3,
                                 std::span(reinterpret_cast<const std::byte*>(text.data()), text.size()));
    const std::string encoded = liquid::to_string(*id);
    return "repositories/" + encoded.substr(encoded.find(':') + 1);
}

void seed_repository(const std::filesystem::path& root, const std::string& customer_id,
                     const std::string& repository_id, const std::string& text,
                     std::size_t extra_bytes, std::uint64_t seed) {
    auto objects = liquid::runtime::block_on(liquid::store::open(root / customer_id / "store"));
    ASSERT_TRUE(objects) << objects.error().message;
    liquid::repository_options options;
    options.store = &*objects;
    auto repository = liquid::runtime::block_on(
        liquid::repository::create(root / customer_id / "repositories" / repository_id, options));
    ASSERT_TRUE(repository) << repository.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(repository->write("README.md", bytes_of(text))));
    if (extra_bytes != 0) {
        ASSERT_TRUE(liquid::runtime::block_on(repository->write("data/blob.bin", random_bytes(extra_bytes, seed))));
    }
    auto change = liquid::runtime::block_on(repository->cut("Seed " + repository_id));
    ASSERT_TRUE(change) << change.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(repository->set_ref("main", *change)));
}

std::map<std::string, std::uintmax_t> files_under(const std::filesystem::path& directory) {
    std::map<std::string, std::uintmax_t> output;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
        if (entry.is_regular_file()) {
            output[std::filesystem::relative(entry.path(), directory).string()] = entry.file_size();
        }
    }
    return output;
}

struct MoveRig {
    std::shared_ptr<lake::control::InMemoryStore> store = std::make_shared<lake::control::InMemoryStore>();
    std::unique_ptr<lake::test::RecordWorkspaces> workspaces;
    std::shared_ptr<lake::test::FakeZettaObjects> zetta = std::make_shared<lake::test::FakeZettaObjects>();
    std::string customer_id;
    std::string alpha;
    std::string beta;
    lake::control::Repository repo_a;
    lake::control::Repository repo_b;
    // A second space with one workspace whose repository stays in that space's store.
    std::string other_customer_id;
    std::string gamma;
    lake::control::Repository repo_c;
    std::filesystem::path root = lake::test::fixture_path("workspace-store-move-" + lake::control::new_uuid_v4());
    lake::api::ServerOptions options;
    std::unique_ptr<lake::api::ApiServer> server;

    void SetUp(std::chrono::milliseconds lease_ttl = 30s) {
        const auto user = store->create_user({"taap:workspace-store", "", "Workspace Store"});
        ASSERT_TRUE(user);
        const auto customer = store->create_customer({"individual", user->id, "", "workspace-store"});
        ASSERT_TRUE(customer);
        customer_id = customer->id;
        workspaces = std::make_unique<lake::test::RecordWorkspaces>(store);
        alpha = workspaces->create(customer_id, "alpha");
        beta = workspaces->create(customer_id, "beta");
        zetta->add_node("http://" + alpha + ".test", "token-" + alpha);
        zetta->add_node("http://" + beta + ".test", "token-" + beta);
        auto created_a = store->create_repository({customer_id, "alpha-source", "", "private", alpha});
        auto created_b = store->create_repository({customer_id, "beta-source", "", "private", beta});
        ASSERT_TRUE(created_a && created_b);
        repo_a = *created_a;
        repo_b = *created_b;
        seed_repository(root, customer_id, repo_a.id, "alpha readme " + std::string(marker) + "\n",
                        2 * 1024 * 1024, 80);
        seed_repository(root, customer_id, repo_b.id, "beta readme\n", 0, 81);
        const auto other_user = store->create_user({"taap:workspace-store-other", "", "Other Space"});
        ASSERT_TRUE(other_user);
        const auto other = store->create_customer({"individual", other_user->id, "", "other-space"});
        ASSERT_TRUE(other);
        other_customer_id = other->id;
        gamma = workspaces->create(other_customer_id, "gamma");
        auto created_c = store->create_repository({other_customer_id, "gamma-source", "", "private", gamma});
        ASSERT_TRUE(created_c);
        repo_c = *created_c;
        seed_repository(root, other_customer_id, repo_c.id, "gamma readme\n", 0, 82);
        options.port = 0;
        options.store = store;
        options.workspaces = workspaces->service;
        options.object_transport = zetta;
        options.workspace_store_lease_ttl = lease_ttl;
        options.admin_token = "workspace-store-operator";
        options.instance_kek = std::string(kek);
        options.store_root = root.string();
        options.search = std::make_shared<lake::search::Service>(root);
        ASSERT_NO_FATAL_FAILURE(start());
    }

    void start() {
        server = std::make_unique<lake::api::ApiServer>(options);
        ASSERT_TRUE(server->start()) << server->reason();
        for (int waited = 0; waited < 200 && !server->store_root_open(); ++waited) {
            std::this_thread::sleep_for(20ms);
        }
        ASSERT_TRUE(server->store_root_open());
    }

    // A new lake process on the same volume and control store: every store is opened again.
    void restart() {
        server->stop();
        server.reset();
        ASSERT_NO_FATAL_FAILURE(start());
    }

    std::pair<unsigned, json> call(const std::string& method, const std::string& path,
                                   const json& body = nullptr,
                                   const std::string& bearer = "workspace-store-operator") {
        const std::string data = body.is_null() ? std::string() : body.dump();
        const auto* bytes = reinterpret_cast<const std::byte*>(data.data());
        const auto response = lake::test::http_request(
            {.url = "http://127.0.0.1:" + std::to_string(server->port()),
             .method = method,
             .suffix = path,
             .headers = {{"Authorization", "Bearer " + bearer},
                         {"Content-Type", "application/json"}},
             .body = std::vector<std::byte>(bytes, bytes + data.size())});
        EXPECT_TRUE(response);
        if (!response) {
            return {0, json::object()};
        }
        const std::string text(reinterpret_cast<const char*>(response->body.data()), response->body.size());
        return {response->status, json::parse(text, nullptr, false)};
    }

    std::pair<unsigned, json> move(const std::string& workspace, const std::string& mode) {
        return call("POST", "/v1/move_workspace_store", {{"workspace_id", workspace}, {"mode", mode}});
    }

    std::vector<std::string> tree(const lake::control::Repository& repository) {
        const auto [status, body] = call("GET", "/v1/repository_tree?id=" + repository.id + "&ref=main");
        EXPECT_EQ(status, 200U) << body.dump();
        std::vector<std::string> paths;
        for (const json& item : body["value"]["items"]) {
            paths.push_back(item.value("path", std::string()));
        }
        std::ranges::sort(paths);
        return paths;
    }

    // Pushes a new local history with one file into `repository` (its first push) through the
    // liquid sync protocol. Answers the error message, empty on success.
    std::string push(const lake::control::Repository& repository, const std::string& path,
                     const std::string& text) {
        const auto local = root.parent_path() / ("push-" + lake::control::new_uuid_v4());
        auto source = liquid::runtime::block_on(liquid::repository::create(local));
        if (!source) {
            return source.error().message;
        }
        if (auto written = liquid::runtime::block_on(source->write(path, bytes_of(text))); !written) {
            return written.error().message;
        }
        auto change = liquid::runtime::block_on(source->cut("Push " + path));
        if (!change) {
            return change.error().message;
        }
        if (auto ref = liquid::runtime::block_on(source->set_ref("main", *change)); !ref) {
            return ref.error().message;
        }
        liquid::sync_endpoint endpoint(
            "http://127.0.0.1:" + std::to_string(server->port()) + "/v1/repositories/" + repository.id,
            liquid::sync_endpoint_options{.bearer_token = "workspace-store-operator"});
        auto pushed = lake::test::block_on_one_thread(endpoint.push(*source));
        std::error_code ignored;
        std::filesystem::remove_all(local, ignored);
        return pushed ? std::string() : pushed.error().message;
    }

    // Creates a repository; nothing (and a test failure) when lake refuses it.
    std::optional<lake::control::Repository> create(const std::string& workspace_id,
                                                    const std::string& name);

    // Commits one file and answers the status and body.
    std::pair<unsigned, json> commit(const lake::control::Repository& repository, const std::string& path,
                                     const std::string& text) {
        static constexpr std::string_view alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string encoded;
        std::size_t index = 0;
        for (; index + 2 < text.size(); index += 3) {
            const unsigned value = (static_cast<unsigned char>(text[index]) << 16U) |
                                   (static_cast<unsigned char>(text[index + 1]) << 8U) |
                                   static_cast<unsigned char>(text[index + 2]);
            encoded += alphabet[(value >> 18U) & 63U];
            encoded += alphabet[(value >> 12U) & 63U];
            encoded += alphabet[(value >> 6U) & 63U];
            encoded += alphabet[value & 63U];
        }
        if (index < text.size()) {
            unsigned value = static_cast<unsigned char>(text[index]) << 16U;
            if (index + 1 < text.size()) {
                value |= static_cast<unsigned char>(text[index + 1]) << 8U;
            }
            encoded += alphabet[(value >> 18U) & 63U];
            encoded += alphabet[(value >> 12U) & 63U];
            encoded += index + 1 < text.size() ? alphabet[(value >> 6U) & 63U] : '=';
            encoded += '=';
        }
        const auto [status, body] = call(
            "POST", "/v1/commit_repository_files",
            {{"id", repository.id},
             {"revision", "main"},
             {"message", "Add " + path},
             {"operations", json::array({{{"type", "file"}, {"path", path}, {"content", encoded}}})}});
        return {status, body};
    }

    ~MoveRig() {
        if (server) {
            server->stop();
        }
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

std::optional<lake::control::Repository> MoveRig::create(const std::string& workspace_id,
                                                         const std::string& name) {
    const auto [status, body] = call("POST", "/v1/create_repository",
                                     {{"customer_id", customer_id}, {"name", name},
                                      {"workspace_id", workspace_id}, {"visibility", "private"}});
    return MoveRig_create(status, body);
}

TEST(WorkspaceStoreMove, PlanCopyInterruptedVerifySwitchServeFromTheWorkspaceAndKeepWorkspacesApart) {
    MoveRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    const std::string endpoint_a = "http://" + rig.alpha + ".test";
    const std::string endpoint_b = "http://" + rig.beta + ".test";
    EXPECT_EQ(rig.tree(rig.repo_a), (std::vector<std::string>{"README.md", "data", "data/blob.bin"}));
    EXPECT_FALSE(rig.zetta->has_bucket(endpoint_a)) << "nothing opens a workspace store before it is needed";

    const auto [plan_status, plan] = rig.move(rig.alpha, "plan");
    ASSERT_EQ(plan_status, 200U) << plan.dump();
    ASSERT_EQ(plan["value"]["repositories"].size(), 1U);
    EXPECT_EQ(plan["value"]["repositories"][0]["id"], rig.repo_a.id);
    EXPECT_GT(plan["value"]["repositories"][0]["logical_bytes"].get<std::uint64_t>(), 2'000'000U);
    EXPECT_TRUE(plan["value"]["unassigned"].empty());
    EXPECT_FALSE(plan["value"]["blocked"].get<bool>());
    EXPECT_TRUE(plan["value"]["store_moved_at"].is_null());

    // The copy is interrupted part way: after twenty requests the workspace Zetta answers none.
    rig.zetta->fail_after(20);
    const auto [interrupted_status, interrupted] = rig.move(rig.alpha, "copy");
    EXPECT_NE(interrupted_status, 200U) << interrupted.dump();
    rig.zetta->fail_after(-1);
    const auto partial = rig.zetta->objects(endpoint_a);
    EXPECT_FALSE(partial.contains(hash_key(rig.root / rig.customer_id / "repositories" / rig.repo_a.id) +
                                  "/LIQUID_REPO"))
        << "an interrupted copy publishes no repository";
    const auto [copy_status, copied] = rig.move(rig.alpha, "copy");
    ASSERT_EQ(copy_status, 200U) << copied.dump();
    EXPECT_TRUE(copied["value"]["complete"].get<bool>());
    const auto [verify_status, verified] = rig.move(rig.alpha, "verify");
    ASSERT_EQ(verify_status, 200U) << verified.dump();
    EXPECT_TRUE(verified["value"]["complete"].get<bool>()) << verified.dump();
    EXPECT_TRUE(verified["value"]["repositories"][0]["differences"].empty());
    const auto [rerun_status, rerun] = rig.move(rig.alpha, "copy");
    ASSERT_EQ(rerun_status, 200U);
    EXPECT_EQ(rerun["value"]["repositories"][0]["objects_written"], 0U) << "a rerun copies nothing twice";
    EXPECT_EQ(rerun["value"]["repositories"][0]["bytes_written"], 0U);

    // Still served from the space store until the switch.
    const auto space_store = rig.root / rig.customer_id / "store";
    const auto space_before = files_under(space_store / "packs");
    const auto [switch_status, switched] = rig.move(rig.alpha, "switch");
    ASSERT_EQ(switch_status, 200U) << switched.dump();
    EXPECT_TRUE(switched["value"]["switched"].get<bool>());
    EXPECT_FALSE(switched["value"]["store_moved_at"].get<std::string>().empty());
    EXPECT_FALSE(rig.store->get_workspace(rig.alpha)->store_moved_at.empty());
    const auto [again_status, again] = rig.move(rig.alpha, "copy");
    EXPECT_EQ(again_status, 409U) << again.dump();

    // Served and written from the workspace store; the space store is left as it was.
    EXPECT_EQ(rig.tree(rig.repo_a), (std::vector<std::string>{"README.md", "data", "data/blob.bin"}));
    const auto [file_status, file] =
        rig.call("GET", "/v1/repository_file?id=" + rig.repo_a.id + "&ref=main&path=README.md");
    ASSERT_EQ(file_status, 200U) << file.dump();
    EXPECT_NE(file.dump().find("alpha readme"), std::string::npos);
    if (written(rig.commit(rig.repo_a, "after-switch.txt", "written after the switch " + std::string(marker)))) {
        EXPECT_EQ(rig.tree(rig.repo_a),
                  (std::vector<std::string>{"README.md", "after-switch.txt", "data", "data/blob.bin"}));
    }
    EXPECT_EQ(files_under(space_store / "packs"), space_before) << "the space store is not written";
    const auto bucket_a = rig.zetta->objects(endpoint_a);
    const std::string state_a = hash_key(rig.root / rig.customer_id / "repositories" / rig.repo_a.id);
    const std::string state_b = hash_key(rig.root / rig.customer_id / "repositories" / rig.repo_b.id);
    EXPECT_TRUE(bucket_a.contains(state_a + "/LIQUID_REPO"));
    EXPECT_FALSE(holds_marker(bucket_a)) << "the workspace Zetta holds only ciphertext";
    EXPECT_TRUE(rig.zetta->lease(endpoint_a).had_holder);
    EXPECT_FALSE(rig.zetta->lease(endpoint_a).holder.empty());

    // The other workspace of the space is untouched, then moves on its own.
    EXPECT_EQ(rig.tree(rig.repo_b), (std::vector<std::string>{"README.md"}));
    EXPECT_FALSE(rig.zetta->has_bucket(endpoint_b));
    ASSERT_EQ(rig.move(rig.beta, "copy").first, 200U);
    ASSERT_EQ(rig.move(rig.beta, "switch").first, 200U);
    if (written(rig.commit(rig.repo_b, "beta.txt", "beta after its switch"))) {
        EXPECT_EQ(rig.tree(rig.repo_b), (std::vector<std::string>{"README.md", "beta.txt"}));
    }
    const auto bucket_b = rig.zetta->objects(endpoint_b);
    const auto bucket_a_after = rig.zetta->objects(endpoint_a);
    EXPECT_TRUE(bucket_b.contains(state_b + "/LIQUID_REPO"));
    EXPECT_FALSE(bucket_b.contains(state_a + "/LIQUID_REPO"));
    EXPECT_FALSE(bucket_a_after.contains(state_b + "/LIQUID_REPO"));
    const std::string key_object(lake::control::workspace_store_key_object);
    EXPECT_NE(bucket_a_after.at(key_object), bucket_b.at(key_object)) << "one data key per workspace";
    EXPECT_FALSE(holds_marker(bucket_b));
}

TEST(WorkspaceStoreMove, ALostLeaseAnswers503UntilReacquiredAndDeletionReleasesTheLease) {
    MoveRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp(900ms));
    const std::string endpoint_a = "http://" + rig.alpha + ".test";
    ASSERT_EQ(rig.move(rig.alpha, "copy").first, 200U);
    ASSERT_EQ(rig.move(rig.alpha, "switch").first, 200U);
    EXPECT_EQ(rig.tree(rig.repo_a), (std::vector<std::string>{"README.md", "data", "data/blob.bin"}));
    const auto held = rig.zetta->lease(endpoint_a);

    // Another process takes the lease: lake's renewal is refused and the workspace answers 503.
    rig.zetta->take_over(endpoint_a, "another lake");
    std::this_thread::sleep_for(1200ms);
    const auto [lost_status, lost] =
        rig.call("GET", "/v1/repository_tree?id=" + rig.repo_a.id + "&ref=main");
    EXPECT_EQ(lost_status, 503U) << lost.dump();
    EXPECT_EQ(lost["error"]["code"], "store_unavailable") << lost.dump();
    // and keeps answering at once, without waiting for the other holder, while it holds the lease.
    const auto again_started = std::chrono::steady_clock::now();
    EXPECT_EQ(rig.call("GET", "/v1/repository_tree?id=" + rig.repo_a.id + "&ref=main").first, 503U);
    EXPECT_LT(std::chrono::steady_clock::now() - again_started, 5s);
    // The other workspace is not affected.
    EXPECT_EQ(rig.tree(rig.repo_b), (std::vector<std::string>{"README.md"}));
    // Once the other holder lets go, the next request owns the store again with a higher fence.
    rig.zetta->release(endpoint_a);
    EXPECT_EQ(rig.tree(rig.repo_a), (std::vector<std::string>{"README.md", "data", "data/blob.bin"}));
    EXPECT_GT(rig.zetta->lease(endpoint_a).fence, held.fence);
    if (written(rig.commit(rig.repo_a, "reacquired.txt", "after the lease came back"))) {
        EXPECT_EQ(rig.tree(rig.repo_a),
                  (std::vector<std::string>{"README.md", "data", "data/blob.bin", "reacquired.txt"}));
    }

    // Deleting the workspace closes its store and releases its lease.
    const auto [deleted_status, deleted] =
        rig.call("POST", "/v1/delete_workspace",
                 {{"customer_id", rig.customer_id}, {"workspace_id", rig.alpha}, {"confirm", "alpha"}});
    ASSERT_EQ(deleted_status, 200U) << deleted.dump();
    EXPECT_TRUE(rig.zetta->lease(endpoint_a).holder.empty()) << deleted.dump();
}

// ---- review fixes: integrity, isolation, renewal -----------------------------------------

std::string tail_of(const std::map<std::string, std::string>& objects, const std::string& key) {
    for (const auto& [name, bytes] : objects) {
        if (name.starts_with(key + ".lake-tail-")) {
            return name;
        }
    }
    return {};
}

TEST(WorkspaceStoreBackend, AMissingKeyIsCreatedOnlyForANewStoreInAnEmptyBucket) {
    Rig rig;
    // Not creating: an empty bucket without a key is refused and nothing is written.
    auto opening = rig.backend(small_blocks(), false);
    const auto refused = liquid::runtime::block_on(opening->put("objects/a", bytes_of("a")));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::corrupt);
    EXPECT_TRUE(rig.zetta->objects(rig.endpoint).empty());

    // Creating: a bucket that holds objects but no key (a wiped key, the wrong bucket) is refused
    // too, and its objects are left as they were.
    auto creator = rig.backend(small_blocks());
    ASSERT_TRUE(liquid::runtime::block_on(creator->put("objects/a", bytes_of("a"))));
    ASSERT_TRUE(liquid::runtime::block_on(creator->sync()));
    rig.zetta->erase_object(rig.endpoint, std::string(lake::control::workspace_store_key_object));
    const auto before = rig.zetta->objects(rig.endpoint);
    auto over = rig.backend(small_blocks(), true);
    const auto minted = liquid::runtime::block_on(over->put("objects/b", bytes_of("b")));
    ASSERT_FALSE(minted);
    EXPECT_EQ(minted.error().code, liquid::errc::corrupt);
    EXPECT_EQ(rig.zetta->objects(rig.endpoint), before);
}

TEST(WorkspaceStoreBackend, ARemoteConflictOnAConditionalCreateFailsTheBackendAndKeepsItsBuffer) {
    Rig rig;
    auto backend = rig.backend(small_blocks());
    const auto pack = random_bytes(1500, 90);
    ASSERT_TRUE(liquid::runtime::block_on(backend->put_range("packs/pack-1.pack", 0, pack)));
    // Another writer, which the fence should exclude, creates the key behind the backend's back.
    rig.zetta->set_object(rig.endpoint, "repositories/x/LIQUID_REPO", "foreign");
    const auto created =
        liquid::runtime::block_on(backend->put_if_absent("repositories/x/LIQUID_REPO", bytes_of("mine")));
    ASSERT_FALSE(created);
    EXPECT_EQ(created.error().code, liquid::errc::already_exists);
    EXPECT_TRUE(backend->failed()) << "the store must be reopened";
    // The buffered append was not dropped.
    ASSERT_TRUE(liquid::runtime::block_on(backend->sync()));
    auto reopened = rig.backend(small_blocks());
    EXPECT_EQ(read(*reopened, "packs/pack-1.pack", 0, pack.size()), pack);
}

TEST(WorkspaceStoreBackend, TailRollbackDeletionTruncationAndACrashBeforeTheTailAreAsDocumented) {
    Rig rig;
    auto backend = rig.backend(small_blocks());
    std::vector<std::byte> grown;
    const auto append = [&](lake::control::ZettaObjectBackend& writer, std::size_t size, std::uint64_t seed) {
        const auto part = random_bytes(size, seed);
        ASSERT_TRUE(liquid::runtime::block_on(writer.put_range("packs/pack-1.pack", grown.size(), part)));
        grown.insert(grown.end(), part.begin(), part.end());
    };
    // An object created and barriered empty, so what follows is appended (an object created and
    // filled in one interval is one PUT, with no tail).
    ASSERT_TRUE(liquid::runtime::block_on(backend->put_range("packs/pack-1.pack", 0, {})));
    ASSERT_TRUE(liquid::runtime::block_on(backend->sync()));
    append(*backend, 1500, 91); // one appended block and a tail of 476 bytes
    ASSERT_TRUE(liquid::runtime::block_on(backend->sync()));
    const auto first = rig.zetta->objects(rig.endpoint);
    const std::string first_tail = tail_of(first, "packs/pack-1.pack");
    ASSERT_FALSE(first_tail.empty());
    append(*backend, 300, 92); // the same block's tail grows to 776 bytes
    ASSERT_TRUE(liquid::runtime::block_on(backend->sync()));
    const std::string second_tail = tail_of(rig.zetta->objects(rig.endpoint), "packs/pack-1.pack");
    EXPECT_EQ(second_tail, first_tail) << "a tail of the same block keeps its name";

    // Rollback of the tail to its earlier, shorter version at the same index: not detected by the
    // cipher (documented); the object reads as the shorter, authentic version.
    rig.zetta->set_object(rig.endpoint, first_tail, first.at(first_tail));
    auto rolled = rig.backend(small_blocks());
    EXPECT_EQ(size_of(*rolled, "packs/pack-1.pack"), 1500U);
    EXPECT_EQ(read(*rolled, "packs/pack-1.pack", 0, 1500),
              std::vector<std::byte>(grown.begin(), grown.begin() + 1500));
    // Deletion of the tail: also undetected; the object ends at its last complete block.
    rig.zetta->erase_object(rig.endpoint, first_tail);
    auto untailed = rig.backend(small_blocks());
    EXPECT_EQ(size_of(*untailed, "packs/pack-1.pack"), 1024U);
    EXPECT_EQ(read(*untailed, "packs/pack-1.pack", 0, 1024),
              std::vector<std::byte>(grown.begin(), grown.begin() + 1024));
    // Truncation inside a block is detected.
    std::string main = rig.zetta->objects(rig.endpoint).at("packs/pack-1.pack");
    rig.zetta->set_object(rig.endpoint, "packs/pack-1.pack", main.substr(0, main.size() - 7));
    auto truncated = rig.backend(small_blocks());
    const auto torn = liquid::runtime::block_on(truncated->get_range("packs/pack-1.pack", 0, 10));
    ASSERT_FALSE(torn);
    EXPECT_EQ(torn.error().code, liquid::errc::corrupt);
    // Truncation to a block boundary drops whole blocks without notice (documented): the header
    // and the base block remain, and reads stay authentic.
    rig.zetta->set_object(rig.endpoint, "packs/pack-1.pack", main.substr(0, main.size() - 1024 - 28));
    auto shortened = rig.backend(small_blocks());
    EXPECT_EQ(size_of(*shortened, "packs/pack-1.pack"), 0U);

    // A crash between a barrier's PATCH and its tail put: everything acknowledged by the earlier
    // barrier is still there.
    Rig crash;
    auto writer = crash.backend(small_blocks());
    std::vector<std::byte> bytes = random_bytes(1500, 93);
    ASSERT_TRUE(liquid::runtime::block_on(writer->put_range("packs/pack-2.pack", 0, {})));
    ASSERT_TRUE(liquid::runtime::block_on(writer->sync()));
    ASSERT_TRUE(liquid::runtime::block_on(writer->put_range("packs/pack-2.pack", 0, bytes)));
    ASSERT_TRUE(liquid::runtime::block_on(writer->sync())); // acknowledged: 1,500 bytes
    const auto more = random_bytes(1000, 94);                  // completes block 1, tail of 452
    ASSERT_TRUE(liquid::runtime::block_on(writer->put_range("packs/pack-2.pack", 1500, more)));
    crash.zetta->drop_on("PUT", 1, 4); // the tail put and its three retries never land
    EXPECT_FALSE(liquid::runtime::block_on(writer->sync()));
    EXPECT_TRUE(writer->failed());
    auto recovered = crash.backend(small_blocks());
    EXPECT_EQ(size_of(*recovered, "packs/pack-2.pack"), 2048U);
    std::vector<std::byte> expected = bytes;
    expected.insert(expected.end(), more.begin(), more.end());
    EXPECT_EQ(read(*recovered, "packs/pack-2.pack", 0, 2048),
              std::vector<std::byte>(expected.begin(), expected.begin() + 2048));
}

TEST(WorkspaceStoreBackend, TailsThatAreNotCurrentAreRemovedAfterOpen) {
    Rig rig;
    auto backend = rig.backend(small_blocks());
    ASSERT_TRUE(liquid::runtime::block_on(backend->put_range("packs/pack-1.pack", 0, {})));
    ASSERT_TRUE(liquid::runtime::block_on(backend->sync()));
    ASSERT_TRUE(liquid::runtime::block_on(backend->put_range("packs/pack-1.pack", 0, random_bytes(1500, 95))));
    ASSERT_TRUE(liquid::runtime::block_on(backend->sync()));
    const std::string current = tail_of(rig.zetta->objects(rig.endpoint), "packs/pack-1.pack");
    // A tail an interrupted barrier left behind, for a block the object already passed.
    const std::string stale = current.substr(0, current.find(".lake-tail-")) + ".lake-tail-0" +
                              current.substr(current.rfind('-'));
    rig.zetta->set_object(rig.endpoint, stale, "left behind");
    auto reopened = rig.backend(small_blocks());
    EXPECT_EQ(size_of(*reopened, "packs/pack-1.pack"), 1500U);
    ASSERT_TRUE(liquid::runtime::block_on(reopened->put("objects/x", bytes_of("x"))));
    ASSERT_TRUE(liquid::runtime::block_on(reopened->sync()));
    const auto after = rig.zetta->objects(rig.endpoint);
    EXPECT_FALSE(after.contains(stale));
    EXPECT_TRUE(after.contains(current));
}

TEST(WorkspaceStoreBackend, AThresholdFlushAndABarrierOnOneKeyDoNotSendItsBlocksTwice) {
    Rig rig;
    auto backend = rig.backend(small_blocks());
    std::vector<std::byte> first = random_bytes(5000, 96); // past the 4 KiB flush threshold
    rig.zetta->hang(rig.endpoint, 300ms, "?offset=");
    std::thread flusher([&] {
        EXPECT_TRUE(liquid::runtime::block_on(backend->put_range("packs/pack-1.pack", 0, first)));
    });
    std::this_thread::sleep_for(100ms);
    EXPECT_TRUE(liquid::runtime::block_on(backend->sync()));
    flusher.join();
    rig.zetta->hang(rig.endpoint, 0ms);
    ASSERT_TRUE(liquid::runtime::block_on(backend->sync()));
    auto reopened = rig.backend(small_blocks());
    EXPECT_EQ(size_of(*reopened, "packs/pack-1.pack"), first.size());
    EXPECT_EQ(read(*reopened, "packs/pack-1.pack", 0, first.size()), first);
}

TEST(WorkspaceStoreOwner, ARenewalThatTimesOutLosesTheLeaseInTime) {
    Rig rig;
    const auto root = lake::test::fixture_path("workspace-store-renew-" + rig.workspace);
    std::shared_ptr<ZettaStoreOwner> owner;
    auto store = liquid::runtime::block_on(liquid::store::open(root, owned(rig, owner, 900ms)));
    ASSERT_TRUE(store) << store.error().message;
    // Renewals hang past the lease; each one gives up after a third of the lease.
    rig.zetta->hang(rig.endpoint, 5s, ":renew");
    const auto started = std::chrono::steady_clock::now();
    while (!owner->lost() && std::chrono::steady_clock::now() - started < 4s) {
        std::this_thread::sleep_for(50ms);
    }
    EXPECT_TRUE(owner->lost());
    EXPECT_LT(std::chrono::steady_clock::now() - started, 3s);
    const auto refused = liquid::runtime::block_on(store->put(random_bytes(100, 97)));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::locked);
    rig.zetta->hang(rig.endpoint, 0ms);
    static_cast<void>(liquid::runtime::block_on(store->close()));
}

TEST(WorkspaceStoreMove, AWipedOrIncompleteWorkspaceStoreIsRefusedNeverServedEmpty) {
    MoveRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    const std::string endpoint_a = "http://" + rig.alpha + ".test";
    ASSERT_EQ(rig.move(rig.alpha, "copy").first, 200U);
    ASSERT_EQ(rig.move(rig.alpha, "switch").first, 200U);
    const std::string state_a = hash_key(rig.root / rig.customer_id / "repositories" / rig.repo_a.id);
    // The switch's search index rebuild (lake#381) opens the repository; let it finish and close
    // every store, so the loss below happens while nothing has the repository open.
    for (int attempt = 0; attempt < 300; ++attempt) {
        const auto [status, body] = rig.call("GET", "/v1/search?query=readme&workspace_id=" + rig.alpha);
        if (status == 200U && body["value"]["building_workspaces"].empty()) {
            break;
        }
        std::this_thread::sleep_for(50ms);
    }
    {
        const auto [status, body] = rig.call("GET", "/v1/search?query=readme&workspace_id=" + rig.alpha);
        ASSERT_EQ(status, 200U) << body.dump();
        ASSERT_TRUE(body["value"]["building_workspaces"].empty()) << "the rebuild did not finish: " << body.dump();
    }
    ASSERT_NO_FATAL_FAILURE(rig.restart());

    // The repository's state is gone from the workspace store: a read is refused as corrupt and
    // nothing is created.
    rig.zetta->erase_object(endpoint_a, state_a + "/LIQUID_REPO");
    ASSERT_NO_FATAL_FAILURE(rig.restart());
    const auto [missing_status, missing] =
        rig.call("GET", "/v1/repository_tree?id=" + rig.repo_a.id + "&ref=main");
    EXPECT_EQ(missing_status, 500U) << missing.dump();
    EXPECT_EQ(missing["error"]["code"], "corrupt") << missing.dump();
    EXPECT_FALSE(rig.zetta->objects(endpoint_a).contains(state_a + "/LIQUID_REPO"));
    const auto pushed = rig.commit(rig.repo_a, "x.txt", "never lands");
    EXPECT_NE(pushed.first, 200U) << pushed.second.dump();
    EXPECT_FALSE(rig.zetta->objects(endpoint_a).contains(state_a + "/LIQUID_REPO"));

    // The whole bucket is wiped (a recreated workspace Zetta): refused, and no new key appears.
    rig.zetta->clear_bucket(endpoint_a);
    ASSERT_NO_FATAL_FAILURE(rig.restart());
    const auto [wiped_status, wiped] =
        rig.call("GET", "/v1/repository_tree?id=" + rig.repo_a.id + "&ref=main");
    EXPECT_EQ(wiped_status, 500U) << wiped.dump();
    EXPECT_EQ(wiped["error"]["code"], "corrupt") << wiped.dump();
    EXPECT_NE(rig.commit(rig.repo_a, "y.txt", "never lands").first, 200U);
    EXPECT_TRUE(rig.zetta->objects(endpoint_a).empty()) << "nothing was created in the wiped bucket";
}

TEST(WorkspaceStoreMove, ADestinationThatLostObjectsNeverVerifiesOrSwitches) {
    MoveRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    const std::string endpoint_a = "http://" + rig.alpha + ".test";
    ASSERT_EQ(rig.move(rig.alpha, "copy").first, 200U);
    for (const auto& [key, bytes] : rig.zetta->objects(endpoint_a)) {
        if (key.starts_with("packs/")) {
            rig.zetta->erase_object(endpoint_a, key);
        }
    }
    ASSERT_NO_FATAL_FAILURE(rig.restart());
    const auto [status, verified] = rig.move(rig.alpha, "verify");
    EXPECT_FALSE(status == 200U && verified["value"]["complete"].get<bool>()) << verified.dump();
    const auto [switch_status, switched] = rig.move(rig.alpha, "switch");
    // The switch copies again first, so it may repair the store; it never switches one that does
    // not verify.
    if (switch_status == 200U) {
        EXPECT_TRUE(switched["value"]["repositories"][0]["complete"].get<bool>()) << switched.dump();
        EXPECT_FALSE(switched["value"]["repositories"][0]["absent"].get<bool>()) << switched.dump();
    }
}

TEST(WorkspaceStoreMove, OtherWorkspacesAndSpacesProceedWhileOneSwitchesOrItsZettaHangs) {
    MoveRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    const std::string endpoint_a = "http://" + rig.alpha + ".test";
    ASSERT_EQ(rig.move(rig.beta, "copy").first, 200U);
    ASSERT_EQ(rig.move(rig.beta, "switch").first, 200U);
    EXPECT_EQ(rig.tree(rig.repo_b), (std::vector<std::string>{"README.md"}));

    // Alpha's switch runs slowly against a slow Zetta, copying from the start.
    rig.zetta->hang(endpoint_a, 60ms);
    std::pair<unsigned, json> switched;
    std::thread switcher([&] { switched = rig.move(rig.alpha, "switch"); });
    std::this_thread::sleep_for(300ms);
    // Forty requests for alpha's repository and a creation in alpha are refused at once.
    std::atomic<int> refused{0};
    std::vector<std::thread> crowd;
    for (int index = 0; index < 40; ++index) {
        crowd.emplace_back([&] {
            const auto [status, body] =
                rig.call("GET", "/v1/repository_tree?id=" + rig.repo_a.id + "&ref=main");
            if (status == 503U && body["error"]["code"] == "store_unavailable") {
                refused.fetch_add(1);
            }
        });
    }
    const auto [creating_status, creating] =
        rig.call("POST", "/v1/create_repository",
                 {{"customer_id", rig.customer_id}, {"name", "created-mid-switch"},
                  {"workspace_id", rig.alpha}, {"visibility", "private"}});
    EXPECT_EQ(creating_status, 503U) << creating.dump();
    // Beta, another workspace with its own store, and the other space answer, push included.
    const auto beta_started = std::chrono::steady_clock::now();
    EXPECT_EQ(rig.tree(rig.repo_b), (std::vector<std::string>{"README.md"}));
    EXPECT_EQ(rig.tree(rig.repo_c), (std::vector<std::string>{"README.md"}));
    const auto beta_new = rig.create(rig.beta, "beta-pushed");
    const bool beta_pushed = beta_new && pushed(rig.push(*beta_new, "pushed.txt", "a push to beta"));
    const auto beta_took = std::chrono::steady_clock::now() - beta_started;
    for (std::thread& thread : crowd) {
        thread.join();
    }
    const bool still_switching = switched.first == 0U;
    switcher.join();
    rig.zetta->hang(endpoint_a, 0ms);
    EXPECT_TRUE(still_switching) << "the switch finished before the checks ran";
    EXPECT_LT(beta_took, 3s) << "beta or the other space waited for alpha's switch";
    EXPECT_EQ(refused.load(), 40);
    ASSERT_EQ(switched.first, 200U) << switched.second.dump();
    if (beta_pushed) {
        EXPECT_EQ(rig.tree(*beta_new), (std::vector<std::string>{"pushed.txt"}));
    }
    // After the switch, the creation and a push go to alpha's workspace store.
    if (const auto alpha_new = rig.create(rig.alpha, "created-after-switch")) {
        EXPECT_TRUE(rig.zetta->objects(endpoint_a).contains(
            hash_key(rig.root / rig.customer_id / "repositories" / alpha_new->id) + "/LIQUID_REPO"));
        if (pushed(rig.push(*alpha_new, "pushed.txt", "a push to alpha's workspace store"))) {
            EXPECT_EQ(rig.tree(*alpha_new), (std::vector<std::string>{"pushed.txt"}));
        }
    }

    // Alpha's Zetta hangs: at most a few of alpha's requests wait, the rest are refused at once,
    // and beta and the other space are not held.
    ASSERT_NO_FATAL_FAILURE(rig.restart()); // alpha's store is not open, so its open will hang
    rig.zetta->hang(endpoint_a, 4s, ":acquire");
    // A space-wide read that reaches alpha's store first (metering_usage reads every
    // repository's builds) is admitted to alpha's lane and waits there: on the pool, not on the
    // server's thread.
    std::thread metering([&] {
        static_cast<void>(rig.call("GET", "/v1/metering_usage?customer_id=" + rig.customer_id));
    });
    std::this_thread::sleep_for(200ms);
    std::atomic<int> waited_out{0};
    std::vector<std::thread> stuck;
    for (int index = 0; index < 40; ++index) {
        stuck.emplace_back([&] {
            const auto [status, body] =
                rig.call("GET", "/v1/repository_tree?id=" + rig.repo_a.id + "&ref=main");
            if (status == 503U) {
                waited_out.fetch_add(1);
            }
        });
    }
    std::this_thread::sleep_for(300ms);
    const auto hung_started = std::chrono::steady_clock::now();
    EXPECT_EQ(rig.tree(rig.repo_b), (std::vector<std::string>{"README.md"}));
    EXPECT_EQ(rig.tree(rig.repo_c), (std::vector<std::string>{"README.md"}));
    if (const auto beta_again = rig.create(rig.beta, "beta-pushed-again")) {
        static_cast<void>(pushed(rig.push(*beta_again, "again.txt", "a push while alpha hangs")));
    }
    EXPECT_LT(std::chrono::steady_clock::now() - hung_started, 3s);
    for (std::thread& thread : stuck) {
        thread.join();
    }
    metering.join();
    rig.zetta->hang(endpoint_a, 0ms);
    EXPECT_GE(waited_out.load(), 36) << "more than the admission limit waited on alpha's lane";
}

TEST(WorkspaceStoreMove, ARepositoryTheSpaceStoreLostIsRefusedNotCreatedEmpty) {
    MoveRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    const std::string endpoint_a = "http://" + rig.alpha + ".test";
    const std::string state = hash_key(rig.root / rig.customer_id / "repositories" / rig.repo_a.id);
    // Lake opens repo_a (which makes its directory beside the space store); then the space store's
    // entry point for it is lost.
    EXPECT_EQ(rig.tree(rig.repo_a), (std::vector<std::string>{"README.md", "data", "data/blob.bin"}));
    std::filesystem::remove(rig.root / rig.customer_id / "store" / state / "LIQUID_REPO");
    const auto [copy_status, copied] = rig.move(rig.alpha, "copy");
    EXPECT_EQ(copy_status, 500U) << copied.dump();
    EXPECT_EQ(copied["error"]["code"], "corrupt") << copied.dump();
    const auto [switch_status, switched] = rig.move(rig.alpha, "switch");
    EXPECT_NE(switch_status, 200U) << switched.dump();
    EXPECT_FALSE(rig.zetta->objects(endpoint_a).contains(state + "/LIQUID_REPO"));
    EXPECT_TRUE(rig.store->get_workspace(rig.alpha)->store_moved_at.empty());
}

TEST(WorkspaceStoreMove, SearchAndReindexRunBesideWritesToTheSameSpaceStore) {
    MoveRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    std::atomic<bool> done{false};
    std::atomic<int> failures{0};
    std::thread searcher([&] {
        while (!done.load()) {
            const auto [status, body] = rig.call(
                "GET", "/v1/search?query=readme&customer=" + rig.customer_id +
                           "&repository=" + rig.repo_b.id + "&history=true");
            if (status != 200U) {
                failures.fetch_add(1);
                ADD_FAILURE() << body.dump();
            }
            const auto [reindexed, reindex] =
                rig.call("POST", "/v1/reindex_artifacts", {{"id", rig.repo_b.id}});
            if (reindexed != 200U) {
                failures.fetch_add(1);
                ADD_FAILURE() << reindex.dump();
            }
        }
    });
    std::vector<std::string> expected{"README.md"};
    for (int index = 0; index < 12; ++index) {
        const std::string path = "file-" + std::to_string(index) + ".txt";
        const auto [status, body] = rig.commit(rig.repo_b, path, "written beside search " + path);
        EXPECT_EQ(status, 200U) << body.dump();
        expected.push_back(path);
    }
    done.store(true);
    searcher.join();
    EXPECT_EQ(failures.load(), 0);
    std::ranges::sort(expected);
    EXPECT_EQ(rig.tree(rig.repo_b), expected);
    // The space store reopens whole: nothing ran ahead of what is on disk.
    ASSERT_NO_FATAL_FAILURE(rig.restart());
    EXPECT_EQ(rig.tree(rig.repo_b), expected);
}

// A write to a repository the switch has already copied, made while the switch is still copying
// another, is refused at once (the store is being moved; retry) instead of landing in the space
// store after its copy, where the switch would lose it. Retried after the switch, it lands in the
// workspace store.
TEST(WorkspaceStoreMove, AWriteDuringTheSwitchIsRefusedAndRetriedIntoTheWorkspaceStore) {
    MoveRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    const std::string endpoint_a = "http://" + rig.alpha + ".test";
    // The newest repository is copied first; repo_a (2 MiB) after it.
    const auto created_early = rig.create(rig.alpha, "alpha-early");
    ASSERT_TRUE(created_early);
    const lake::control::Repository early = *created_early;
    ASSERT_EQ(rig.commit(early, "first.txt", "before the switch").first, 200U);
    const std::string early_state =
        hash_key(rig.root / rig.customer_id / "repositories" / early.id) + "/LIQUID_REPO";

    rig.zetta->hang(endpoint_a, 150ms);
    std::pair<unsigned, json> switched;
    std::thread switcher([&] { switched = rig.move(rig.alpha, "switch"); });
    const auto started = std::chrono::steady_clock::now();
    while (!rig.zetta->objects(endpoint_a).contains(early_state) &&
           std::chrono::steady_clock::now() - started < 60s) {
        std::this_thread::sleep_for(20ms);
    }
    ASSERT_TRUE(rig.zetta->objects(endpoint_a).contains(early_state));
    const auto refused_started = std::chrono::steady_clock::now();
    const auto refused = rig.commit(early, "late.txt", "written during the switch");
    EXPECT_LT(std::chrono::steady_clock::now() - refused_started, 1s) << "the write did not wait";
    EXPECT_EQ(refused.first, 503U) << refused.second.dump();
    EXPECT_EQ(refused.second["error"]["code"], "store_unavailable") << refused.second.dump();
    switcher.join();
    rig.zetta->hang(endpoint_a, 0ms);
    ASSERT_EQ(switched.first, 200U) << switched.second.dump();
    if (written(rig.commit(early, "late.txt", "retried after the switch"))) {
        EXPECT_EQ(rig.tree(early), (std::vector<std::string>{"first.txt", "late.txt"}));
    }
}

TEST(WorkspaceStoreMove, APushLandsInTheWorkspaceStore) {
    MoveRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    const std::string endpoint_a = "http://" + rig.alpha + ".test";
    ASSERT_EQ(rig.move(rig.alpha, "switch").first, 200U);
    const auto target = rig.create(rig.alpha, "pushed-into-workspace");
    if (!target) {
        return; // create() recorded the failure
    }
    const auto before = files_under(rig.root / rig.customer_id / "store" / "packs");
    if (pushed(rig.push(*target, "src/main.txt", "pushed " + std::string(marker)))) {
        EXPECT_EQ(rig.tree(*target), (std::vector<std::string>{"src", "src/main.txt"}));
        EXPECT_FALSE(holds_marker(rig.zetta->objects(endpoint_a)));
    }
    EXPECT_EQ(files_under(rig.root / rig.customer_id / "store" / "packs"), before);
}

TEST(WorkspaceStoreMove, ACreationTheWorkspaceStoreRefusesLeavesNoRowAndCanBeRetried) {
    MoveRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    const std::string endpoint_a = "http://" + rig.alpha + ".test";
    ASSERT_EQ(rig.move(rig.alpha, "switch").first, 200U);
    // Another process holds alpha's lease and the store is not open: the creation's store step
    // fails after the row was written.
    rig.options.store_owner_wait = 500ms;
    ASSERT_NO_FATAL_FAILURE(rig.restart());
    rig.zetta->take_over(endpoint_a, "another lake");
    const auto [failed_status, failed] =
        rig.call("POST", "/v1/create_repository",
                 {{"customer_id", rig.customer_id}, {"name", "retried"}, {"workspace_id", rig.alpha},
                  {"visibility", "private"}});
    EXPECT_NE(failed_status, 200U) << failed.dump();
    const auto rows = rig.store->list_repositories(rig.customer_id, 100, rig.alpha);
    ASSERT_TRUE(rows);
    bool written_row = false;
    for (const auto& row : *rows) {
        if (row.name == "retried") {
            written_row = true;
            EXPECT_FALSE(row.deleted_at.empty()) << "the row was rolled back";
        }
    }
    EXPECT_TRUE(written_row) << "the row is written before the store step";
    rig.zetta->release(endpoint_a);
    // The name is free again and the retry is created in the workspace store.
    if (const auto retried = rig.create(rig.alpha, "retried")) {
        EXPECT_TRUE(rig.zetta->objects(endpoint_a).contains(
            hash_key(rig.root / rig.customer_id / "repositories" / retried->id) + "/LIQUID_REPO"));
    }
}

TEST(WorkspaceStoreMove, ACopyDoesNotRefuseTheSpacesOtherWorkspaces) {
    MoveRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    const std::string endpoint_a = "http://" + rig.alpha + ".test";
    rig.zetta->hang(endpoint_a, 60ms);
    std::pair<unsigned, json> copied;
    std::atomic<bool> copy_done{false};
    std::thread copier([&] {
        copied = rig.move(rig.alpha, "copy");
        copy_done.store(true);
    });
    std::this_thread::sleep_for(300ms);
    // Beta, unswitched in the same space, is served by the space store: it waits at most for the
    // repository being copied and is never refused.
    const bool copying_when_asked = !copy_done.load();
    const auto [status, body] = rig.call("GET", "/v1/repository_tree?id=" + rig.repo_b.id + "&ref=main");
    copier.join();
    rig.zetta->hang(endpoint_a, 0ms);
    EXPECT_TRUE(copying_when_asked) << "the copy finished before beta asked";
    EXPECT_EQ(status, 200U) << body.dump();
    EXPECT_EQ(copied.first, 200U) << copied.second.dump();
}

TEST(WorkspaceStoreMove, ACopyIntoADroppedBucketFailsInsteadOfReportingAbsent) {
    MoveRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    const std::string endpoint_a = "http://" + rig.alpha + ".test";
    ASSERT_EQ(rig.move(rig.alpha, "copy").first, 200U);
    // The workspace Zetta loses the bucket; lake still has the store open.
    rig.zetta->drop_bucket(endpoint_a);
    ASSERT_EQ(rig.commit(rig.repo_a, "after-copy.txt", "new content in the space store").first, 200U);
    const auto [status, body] = rig.move(rig.alpha, "copy");
    EXPECT_NE(status, 200U) << body.dump();
    if (status == 200U) {
        EXPECT_FALSE(body["value"]["repositories"][0]["absent"].get<bool>()) << body.dump();
    }
}

// ---- search beside the workspace store (lake#381) ---------------------------------------------

constexpr std::string_view alpha_term = "alphaterm381";
constexpr std::string_view beta_term = "betaterm381";

// The search index objects of a bucket.
std::map<std::string, std::string> search_objects(lake::test::FakeZettaObjects& zetta,
                                                  const std::string& endpoint) {
    std::map<std::string, std::string> output;
    for (const auto& [key, bytes] : zetta.objects(endpoint)) {
        if (key.starts_with(lake::search::workspace_index_prefix)) {
            output.emplace(key, bytes);
        }
    }
    return output;
}

bool holds_text(const std::map<std::string, std::string>& objects, std::string_view text) {
    return std::ranges::any_of(objects, [&](const auto& object) {
        return object.second.find(text) != std::string::npos;
    });
}

// Every file under the space's index directory on lake's volume, with its contents.
std::string space_index_text(const std::filesystem::path& directory) {
    std::string output;
    if (!std::filesystem::exists(directory)) {
        return output;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
        if (entry.is_regular_file()) {
            std::ifstream file(entry.path(), std::ios::binary);
            output += entry.path().filename().string() + "\n";
            output += std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        }
    }
    return output;
}

std::set<std::string> workspaces_of(const json& body) {
    std::set<std::string> output;
    for (const json& item : body["value"]["items"]) {
        output.insert(item.value("workspace_id", std::string()));
    }
    return output;
}

// Searches until no workspace of the answer is still being built (the rebuild runs on lake's
// maintenance thread), within 15 s.
json searched(MoveRig& rig, const std::string& query, const std::string& bearer = "workspace-store-operator") {
    const auto started = std::chrono::steady_clock::now();
    for (;;) {
        const auto [status, body] = rig.call("GET", "/v1/search?query=" + query, nullptr, bearer);
        EXPECT_EQ(status, 200U) << body.dump();
        if (status != 200U || body["value"]["building_workspaces"].empty() ||
            std::chrono::steady_clock::now() - started > 15s) {
            EXPECT_TRUE(body["value"]["building_workspaces"].empty()) << "still building: " << body.dump();
            return body;
        }
        std::this_thread::sleep_for(50ms);
    }
}

TEST(WorkspaceStoreSearch, TwoWorkspacesOfASpaceKeepSealedIndexesBesideTheirStoresAndARestartReloadsThem) {
    MoveRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    const std::string endpoint_a = "http://" + rig.alpha + ".test";
    const std::string endpoint_b = "http://" + rig.beta + ".test";
    // Before the switch, a history scan puts alpha's text into the space's index on lake's volume.
    const auto alpha_text = rig.create(rig.alpha, "alpha-text");
    ASSERT_TRUE(alpha_text);
    ASSERT_EQ(rig.commit(*alpha_text, "README.md", "alpha readme " + std::string(alpha_term)).first, 200U);
    const auto [scanned, scan] = rig.call("GET", "/v1/search?query=" + std::string(alpha_term) +
                                                     "&customer=" + rig.customer_id + "&repository=" +
                                                     alpha_text->id + "&history=true");
    ASSERT_EQ(scanned, 200U) << scan.dump();
    ASSERT_NE(scan.dump().find("README.md"), std::string::npos);
    const auto space_index = rig.root / rig.customer_id / "index";
    ASSERT_NE(space_index_text(space_index).find(alpha_term), std::string::npos);

    // The switch drops it from the space's index and builds the workspace's index in the
    // background; nothing of either switched workspace is written to the volume afterwards.
    ASSERT_EQ(rig.move(rig.alpha, "copy").first, 200U);
    const auto [switched_a, switch_a] = rig.move(rig.alpha, "switch");
    ASSERT_EQ(switched_a, 200U) << switch_a.dump();
    EXPECT_TRUE(switch_a["value"].contains("space_index_repositories_dropped")) << switch_a.dump();
    EXPECT_EQ(space_index_text(space_index).find(alpha_term), std::string::npos);
    ASSERT_EQ(rig.move(rig.beta, "copy").first, 200U);
    ASSERT_EQ(rig.move(rig.beta, "switch").first, 200U);
    const auto beta_text = rig.create(rig.beta, "beta-text");
    ASSERT_TRUE(beta_text);
    ASSERT_EQ(rig.commit(*beta_text, "README.md", "beta readme " + std::string(beta_term)).first, 200U);
    ASSERT_EQ(rig.call("POST", "/v1/reindex_artifacts", {{"id", beta_text->id}}).first, 200U);

    const auto alpha_hits = searched(rig, std::string(alpha_term) + "&customer=" + rig.customer_id);
    EXPECT_EQ(workspaces_of(alpha_hits), (std::set<std::string>{rig.alpha})) << alpha_hits.dump();
    const auto beta_hits = searched(rig, std::string(beta_term) + "&customer=" + rig.customer_id);
    EXPECT_EQ(workspaces_of(beta_hits), (std::set<std::string>{rig.beta})) << beta_hits.dump();
    EXPECT_TRUE(searched(rig, std::string(alpha_term) + "&workspace_id=" + rig.beta)["value"]["items"].empty());
    EXPECT_TRUE(searched(rig, std::string(beta_term) + "&workspace_id=" + rig.alpha)["value"]["items"].empty());
    EXPECT_EQ(searched(rig, std::string(alpha_term) + "&workspace_id=" + rig.alpha)["value"]["items"].size(), 1U);

    // The indexes are sealed objects beside each store: no term in the clear in either Zetta,
    // and none on lake's volume.
    const auto objects_a = search_objects(*rig.zetta, endpoint_a);
    const auto objects_b = search_objects(*rig.zetta, endpoint_b);
    EXPECT_TRUE(objects_a.contains("search/manifest.json"));
    EXPECT_TRUE(objects_b.contains("search/manifest.json"));
    EXPECT_FALSE(objects_a.contains("search/blobs.json")) << "blobs are in write-once runs";
    for (const std::string_view term : {alpha_term, beta_term, std::string_view("readme")}) {
        EXPECT_FALSE(holds_text(rig.zetta->objects(endpoint_a), term)) << term;
        EXPECT_FALSE(holds_text(rig.zetta->objects(endpoint_b), term)) << term;
    }
    EXPECT_EQ(space_index_text(space_index).find(alpha_term), std::string::npos);
    EXPECT_EQ(space_index_text(space_index).find(beta_term), std::string::npos);

    // A restarted lake loads them from the workspace Zettas and rewrites nothing.
    ASSERT_NO_FATAL_FAILURE(rig.restart());
    const auto [reloaded_status, reloaded] =
        rig.call("GET", "/v1/search?query=" + std::string(alpha_term) + "&customer=" + rig.customer_id);
    ASSERT_EQ(reloaded_status, 200U);
    EXPECT_TRUE(reloaded["value"]["building_workspaces"].empty()) << reloaded.dump();
    EXPECT_EQ(workspaces_of(reloaded), (std::set<std::string>{rig.alpha})) << reloaded.dump();
    EXPECT_EQ(search_objects(*rig.zetta, endpoint_a), objects_a) << "a reload is not a rebuild";
    // The other space's unswitched repository is still searched from its space's index.
    const auto [other_status, other] =
        rig.call("GET", "/v1/search?query=gamma&customer=" + rig.other_customer_id +
                            "&repository=" + rig.repo_c.id + "&history=true");
    ASSERT_EQ(other_status, 200U) << other.dump();
    EXPECT_NE(other.dump().find("README.md"), std::string::npos);
    // A deleted repository leaves its workspace's index.
    ASSERT_EQ(rig.call("POST", "/v1/delete_repository", {{"id", alpha_text->id}}).first, 200U);
    EXPECT_TRUE(searched(rig, std::string(alpha_term) + "&workspace_id=" + rig.alpha)["value"]["items"].empty());
    ASSERT_NO_FATAL_FAILURE(rig.restart());
    EXPECT_TRUE(searched(rig, std::string(alpha_term) + "&workspace_id=" + rig.alpha)["value"]["items"].empty());
}

// lake#381 review B1: who may make search open a workspace.
TEST(WorkspaceStoreSearch, ASearchNeverOpensAWorkspaceItsCallerCannotRead) {
    MoveRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    const std::string endpoint_gamma = "http://" + rig.gamma + ".test";
    rig.zetta->add_node(endpoint_gamma, "token-" + rig.gamma);
    ASSERT_EQ(rig.move(rig.alpha, "switch").first, 200U);
    ASSERT_EQ(rig.move(rig.gamma, "switch").first, 200U);
    const auto alpha_text = rig.create(rig.alpha, "alpha-text");
    ASSERT_TRUE(alpha_text);
    ASSERT_EQ(rig.commit(*alpha_text, "README.md", "shared381 alpha").first, 200U);
    // Both indexes are built, then a restart closes every store: any later touch of gamma's
    // Zetta would be a new request to it.
    EXPECT_EQ(searched(rig, "shared381&customer=" + rig.customer_id)["value"]["items"].size(), 1U);
    EXPECT_EQ(searched(rig, "gamma&workspace_id=" + rig.gamma)["value"]["items"].size(), 1U);
    ASSERT_NO_FATAL_FAILURE(rig.restart());
    const auto token_for = [&](const std::string& taap, const std::string& email) {
        const auto user = rig.store->create_user({taap, email, "Searcher"});
        EXPECT_TRUE(user);
        const auto token = rig.store->create_token({user->id, "lake", "search", "test", {"*"}, 300, false});
        EXPECT_TRUE(token) << token.error().message;
        return std::pair{*user, token->secret};
    };
    const auto space = rig.store->find_policy_object("customer", rig.customer_id);
    ASSERT_TRUE(space);
    // The first space's owner (granted as sign-up provisioning grants it; the in-memory store does
    // not provision), and a person granted read on it who owns no space.
    const auto owner = rig.store->get_customer(rig.customer_id);
    ASSERT_TRUE(owner);
    const auto owner_user = rig.store->get_user(owner->user_id);
    ASSERT_TRUE(owner_user);
    ASSERT_TRUE(rig.store->create_grant({owner_user->taap_id, space->id, "read", "sha512-256:owner", "admin", "grant"}));
    const auto owner_token = rig.store->create_token({owner_user->id, "lake", "search", "test", {"*"}, 300, false});
    ASSERT_TRUE(owner_token) << owner_token.error().message;
    const auto [grantee, grantee_token] = token_for("taap:1:ed25519:search-grantee", "grantee@example.test");
    ASSERT_TRUE(rig.store->create_grant({grantee.taap_id, space->id, "read", "sha512-256:grantee", "admin", "grant"}));
    const std::uint64_t gamma_requests = rig.zetta->requests_to(endpoint_gamma);
    const std::uint64_t gamma_fence = rig.zetta->lease(endpoint_gamma).fence;

    for (const std::string& bearer : {owner_token->secret, grantee_token}) {
        // Without a customer: the spaces the caller may read, owned or granted.
        const auto mine = searched(rig, "shared381", bearer);
        EXPECT_EQ(mine["value"]["items"].size(), 1U) << mine.dump();
        EXPECT_EQ(mine.dump().find(rig.gamma), std::string::npos) << mine.dump();
        EXPECT_EQ(searched(rig, "shared381&customer=" + rig.customer_id, bearer)["value"]["items"].size(), 1U);
        // The other space by name: its index on the volume is read hit by hit; its workspaces are
        // not opened.
        const auto [other_status, other] =
            rig.call("GET", "/v1/search?query=gamma&customer=" + rig.other_customer_id, nullptr, bearer);
        ASSERT_EQ(other_status, 200U) << other.dump();
        EXPECT_TRUE(other["value"]["items"].empty());
        EXPECT_EQ(other.dump().find(rig.gamma), std::string::npos) << other.dump();
        // Another space's workspace and a workspace that does not exist answer the same.
        const auto [named_status, named] =
            rig.call("GET", "/v1/search?query=gamma&workspace_id=" + rig.gamma, nullptr, bearer);
        const auto [unknown_status, unknown] = rig.call(
            "GET", "/v1/search?query=gamma&workspace_id=" + lake::control::new_uuid_v4(), nullptr, bearer);
        EXPECT_EQ(named_status, 400U) << named.dump();
        EXPECT_EQ(named_status, unknown_status);
        EXPECT_EQ(named["error"], unknown["error"]);
        // The other space's repository by id: refused before its workspace is reached.
        const auto [repository_status, repository] = rig.call(
            "GET", "/v1/search?query=gamma&repository=" + rig.repo_c.id, nullptr, bearer);
        EXPECT_EQ(repository_status, 400U) << repository.dump();
    }
    // Not one request reached gamma's Zetta.
    EXPECT_EQ(rig.zetta->requests_to(endpoint_gamma), gamma_requests);
    EXPECT_EQ(rig.zetta->lease(endpoint_gamma).fence, gamma_fence);
    // The operator still reaches it.
    EXPECT_EQ(searched(rig, "gamma&workspace_id=" + rig.gamma)["value"]["items"].size(), 1U);
    EXPECT_GT(rig.zetta->requests_to(endpoint_gamma), gamma_requests);
}

TEST(WorkspaceStoreSearch, AWipedOrTamperedWorkspaceIndexIsRebuiltNotServedEmpty) {
    MoveRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    const std::string endpoint_a = "http://" + rig.alpha + ".test";
    ASSERT_EQ(rig.move(rig.alpha, "switch").first, 200U);
    ASSERT_EQ(rig.move(rig.beta, "switch").first, 200U);
    const auto text = rig.create(rig.alpha, "alpha-text");
    ASSERT_TRUE(text);
    ASSERT_EQ(rig.commit(*text, "README.md", "alpha readme " + std::string(alpha_term)).first, 200U);
    const std::string query = std::string(alpha_term) + "&workspace_id=" + rig.alpha;
    const std::string other = "readme&workspace_id=" + rig.beta;
    ASSERT_EQ(searched(rig, query)["value"]["items"].size(), 1U);
    // Without a restart: under a tight bound a search of beta evicts alpha's index, so alpha's
    // next search reads its objects again.
    rig.options.search->set_memory_bound(1);
    const auto lose = std::vector<std::pair<std::string, std::function<void()>>>{
        {"wiped", [&] {
             for (const auto& [key, bytes] : search_objects(*rig.zetta, endpoint_a)) {
                 rig.zetta->erase_object(endpoint_a, key);
             }
         }},
        {"blob runs lost, manifest kept", [&] {
             for (const auto& [key, bytes] : search_objects(*rig.zetta, endpoint_a)) {
                 if (key.starts_with("search/runs/")) {
                     rig.zetta->erase_object(endpoint_a, key);
                 }
             }
         }},
        {"a run tampered", [&] {
             for (auto [key, bytes] : search_objects(*rig.zetta, endpoint_a)) {
                 if (key.starts_with("search/runs/")) {
                     bytes[bytes.size() - 5] = static_cast<char>(bytes[bytes.size() - 5] ^ 0x01);
                     rig.zetta->set_object(endpoint_a, key, bytes);
                 }
             }
         }},
    };
    for (const auto& [name, damage] : lose) {
        ASSERT_EQ(searched(rig, other)["value"]["items"].size(), 1U) << name;
        damage();
        const auto [status, body] = rig.call("GET", "/v1/search?query=" + query);
        ASSERT_EQ(status, 200U) << name << body.dump();
        EXPECT_TRUE(body["value"]["items"].empty()) << name;
        EXPECT_EQ(body["value"]["building_workspaces"], json::array({rig.alpha})) << name << body.dump();
        EXPECT_EQ(searched(rig, query)["value"]["items"].size(), 1U) << name << ": rebuilt";
    }
    // And across a restart.
    for (const auto& [key, bytes] : search_objects(*rig.zetta, endpoint_a)) {
        rig.zetta->erase_object(endpoint_a, key);
    }
    ASSERT_NO_FATAL_FAILURE(rig.restart());
    EXPECT_EQ(searched(rig, query)["value"]["items"].size(), 1U);
    EXPECT_TRUE(search_objects(*rig.zetta, endpoint_a).contains("search/manifest.json"));
}

// lake#381 review S1: a failed search write is the search's failure, not the store's.
TEST(WorkspaceStoreSearch, AFailedSearchWriteDoesNotFailTheWorkspaceStore) {
    MoveRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    const std::string endpoint_a = "http://" + rig.alpha + ".test";
    ASSERT_EQ(rig.move(rig.alpha, "switch").first, 200U);
    const auto text = rig.create(rig.alpha, "alpha-text");
    ASSERT_TRUE(text);
    ASSERT_EQ(rig.commit(*text, "README.md", "first " + std::string(alpha_term)).first, 200U);
    ASSERT_EQ(searched(rig, std::string(alpha_term) + "&workspace_id=" + rig.alpha)["value"]["items"].size(), 1U);
    ASSERT_EQ(rig.commit(*text, "later.txt", "second laterterm").first, 200U);
    const auto fence = rig.zetta->lease(endpoint_a).fence;
    const auto holder = rig.zetta->lease(endpoint_a).holder;
    // The next object put (the reindex's first index object) and its retries get no answer.
    rig.zetta->drop_on("PUT", 1, 4);
    const auto [failed_status, failed] = rig.call("POST", "/v1/reindex_artifacts", {{"id", text->id}});
    EXPECT_NE(failed_status, 200U) << failed.dump();
    // The store is not reopened: the same lease and holder serve the next request.
    EXPECT_EQ(rig.tree(*text), (std::vector<std::string>{"README.md", "later.txt"}));
    EXPECT_EQ(rig.zetta->lease(endpoint_a).fence, fence);
    EXPECT_EQ(rig.zetta->lease(endpoint_a).holder, holder);
    // The index still answers from what was last written, and the next reindex catches up.
    EXPECT_EQ(searched(rig, std::string(alpha_term) + "&workspace_id=" + rig.alpha)["value"]["items"].size(), 1U);
    ASSERT_EQ(rig.call("POST", "/v1/reindex_artifacts", {{"id", text->id}}).first, 200U);
    EXPECT_EQ(searched(rig, "laterterm&workspace_id=" + rig.alpha)["value"]["items"].size(), 1U);
    EXPECT_EQ(rig.zetta->lease(endpoint_a).fence, fence);
}

// lake#381 review S3: the index is built off the request path.
TEST(WorkspaceStoreSearch, TheFirstPushAfterASwitchDoesNotWaitForTheIndex) {
    MoveRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    const std::string endpoint_a = "http://" + rig.alpha + ".test";
    const auto pushed = rig.create(rig.alpha, "pushed");
    ASSERT_TRUE(pushed);
    // Every index object write takes 400 ms, so building the index takes seconds.
    rig.zetta->hang(endpoint_a, 400ms, "/search/");
    ASSERT_EQ(rig.move(rig.alpha, "switch").first, 200U);
    const auto started = std::chrono::steady_clock::now();
    EXPECT_TRUE(rig.push(*pushed, "pushed.txt", "firstpush381 lands").empty());
    const auto took = std::chrono::steady_clock::now() - started;
    EXPECT_LT(took, 1500ms) << "the push waited for the index";
    // A search meanwhile says the index is building instead of waiting on the workspace.
    const auto search_started = std::chrono::steady_clock::now();
    const auto [status, body] = rig.call("GET", "/v1/search?query=firstpush381&workspace_id=" + rig.alpha);
    EXPECT_LT(std::chrono::steady_clock::now() - search_started, 1s);
    ASSERT_EQ(status, 200U) << body.dump();
    EXPECT_EQ(body["value"]["building_workspaces"], json::array({rig.alpha})) << body.dump();
    rig.zetta->hang(endpoint_a, 0ms, "/search/");
    // Once built, the push made meanwhile is in it.
    EXPECT_EQ(searched(rig, "firstpush381&workspace_id=" + rig.alpha)["value"]["items"].size(), 1U);
}

TEST(WorkspaceStoreSearch, PushesAndSearchesInOneWorkspaceRunSideBySide) {
    MoveRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    const std::string endpoint_a = "http://" + rig.alpha + ".test";
    ASSERT_EQ(rig.move(rig.alpha, "switch").first, 200U);
    std::vector<lake::control::Repository> pushed;
    for (int index = 0; index < 6; ++index) {
        const auto created = rig.create(rig.alpha, "pushed-" + std::to_string(index));
        ASSERT_TRUE(created);
        pushed.push_back(*created);
    }
    const auto text = rig.create(rig.alpha, "alpha-text");
    ASSERT_TRUE(text);
    searched(rig, "pushterm&workspace_id=" + rig.alpha);
    std::atomic<bool> done{false};
    std::atomic<int> failures{0};
    std::atomic<int> answered{0};
    std::atomic<int> busy{0};
    std::thread searcher([&] {
        while (!done.load()) {
            for (const std::string& query :
                 {"/v1/search?query=pushterm&customer=" + rig.customer_id,
                  "/v1/search?query=pushterm&workspace_id=" + rig.alpha,
                  "/v1/search?query=written&customer=" + rig.customer_id + "&repository=" + text->id +
                      "&history=true"}) {
                const auto [status, body] = rig.call("GET", query);
                if (status == 200U) {
                    answered.fetch_add(1);
                } else if (status == 503U) {
                    // A lane at its admission limit answers at once; counted and bounded below.
                    busy.fetch_add(1);
                } else {
                    failures.fetch_add(1);
                    ADD_FAILURE() << status << " " << body.dump();
                }
            }
        }
    });
    std::thread committer([&] {
        for (int index = 0; index < 8; ++index) {
            const std::string path = "file-" + std::to_string(index) + ".txt";
            const auto [status, body] = rig.commit(*text, path, "written beside search " + path);
            if (status != 200U) {
                failures.fetch_add(1);
                ADD_FAILURE() << body.dump();
            }
        }
    });
    for (std::size_t index = 0; index < pushed.size(); ++index) {
        const std::string error = rig.push(pushed[index], "pushed.txt",
                                           "pushterm number" + std::to_string(index) + " lands");
        EXPECT_TRUE(error.empty()) << error;
    }
    committer.join();
    done.store(true);
    searcher.join();
    EXPECT_EQ(failures.load(), 0);
    EXPECT_GT(answered.load(), 0);
    EXPECT_LE(busy.load() * 4, answered.load() + busy.load())
        << "more than a quarter of the searches were refused as busy: " << busy.load() << " of "
        << answered.load() + busy.load();
    // Each push reindexed its repository in the workspace's index.
    const auto body = searched(rig, "pushterm&workspace_id=" + rig.alpha + "&limit=100");
    std::set<std::string> repositories;
    for (const json& item : body["value"]["items"]) {
        repositories.insert(item.value("repository", std::string()));
    }
    EXPECT_EQ(repositories.size(), pushed.size()) << body.dump();
    ASSERT_EQ(rig.call("POST", "/v1/reindex_artifacts", {{"id", text->id}}).first, 200U);
    EXPECT_EQ(searched(rig, "written&workspace_id=" + rig.alpha + "&limit=100")["value"]["items"].size(), 8U);
    EXPECT_FALSE(holds_text(rig.zetta->objects(endpoint_a), "pushterm"));
    ASSERT_NO_FATAL_FAILURE(rig.restart());
    EXPECT_EQ(searched(rig, "pushterm&workspace_id=" + rig.alpha + "&limit=100")["value"]["items"].size(),
              pushed.size());
}

TEST(WorkspaceStoreSearch, DeletingAWorkspaceRemovesItsIndexObjectsAndACleanupFailureDoesNotHoldItUp) {
    MoveRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    const std::string endpoint_a = "http://" + rig.alpha + ".test";
    const std::string endpoint_b = "http://" + rig.beta + ".test";
    ASSERT_EQ(rig.move(rig.alpha, "switch").first, 200U);
    ASSERT_EQ(rig.move(rig.beta, "switch").first, 200U);
    const auto text = rig.create(rig.alpha, "alpha-text");
    ASSERT_TRUE(text);
    ASSERT_EQ(rig.commit(*text, "README.md", "alpha readme " + std::string(alpha_term)).first, 200U);
    ASSERT_EQ(searched(rig, std::string(alpha_term) + "&customer=" + rig.customer_id)["value"]["items"].size(), 1U);
    ASSERT_FALSE(search_objects(*rig.zetta, endpoint_a).empty());
    const auto [status, body] = rig.call("POST", "/v1/delete_workspace",
                                         {{"customer_id", rig.customer_id},
                                          {"workspace_id", rig.alpha},
                                          {"confirm", "alpha"}});
    ASSERT_EQ(status, 200U) << body.dump();
    EXPECT_TRUE(search_objects(*rig.zetta, endpoint_a).empty()) << "the index went with the workspace";
    const auto after = searched(rig, std::string(alpha_term) + "&customer=" + rig.customer_id);
    EXPECT_TRUE(after["value"]["items"].empty());
    EXPECT_EQ(rig.call("GET", "/v1/search?query=" + std::string(alpha_term) + "&workspace_id=" + rig.alpha)
                  .first,
              400U);
    // The search index's cleanup failing (every object removal unanswered) never holds the
    // deletion up: it is logged, and the objects go with the workspace Zetta.
    ASSERT_FALSE(search_objects(*rig.zetta, endpoint_b).empty());
    rig.zetta->drop_on("DELETE", 1, 4);
    const auto [beta_status, beta] = rig.call("POST", "/v1/delete_workspace",
                                              {{"customer_id", rig.customer_id},
                                               {"workspace_id", rig.beta},
                                               {"confirm", "beta"}});
    ASSERT_EQ(beta_status, 200U) << beta.dump();
    EXPECT_TRUE(beta["value"]["deletion"].value("completed", false)) << beta.dump();
}

// The space's stored bytes as lake's quota check counts them (metering_usage).
std::uint64_t space_bytes(MoveRig& rig) {
    const auto [status, body] = rig.call("GET", "/v1/metering_usage?customer_id=" + rig.customer_id);
    EXPECT_EQ(status, 200U) << body.dump();
    const std::string text = body.dump();
    const auto at = text.find("\"repositories_bytes\":");
    EXPECT_NE(at, std::string::npos) << text;
    return at == std::string::npos ? 0 : std::stoull(text.substr(at + std::string("\"repositories_bytes\":").size()));
}

// Runs lake's periodic work (as its main loop does; the usage refresh is due every 10 s) until a
// usage reading counts at least `bytes`, for at most 15 s.
std::uint64_t space_bytes_reaching(MoveRig& rig, std::uint64_t bytes) {
    const auto started = std::chrono::steady_clock::now();
    rig.server->drain();
    std::uint64_t counted = space_bytes(rig);
    while (counted < bytes && std::chrono::steady_clock::now() - started < 15s) {
        std::this_thread::sleep_for(100ms);
        rig.server->drain();
        counted = space_bytes(rig);
    }
    return counted;
}

// Runs lake's periodic work until the next usage reading changes what is counted (at most 15 s).
std::uint64_t next_space_bytes(MoveRig& rig) {
    const std::uint64_t current = space_bytes(rig);
    const auto started = std::chrono::steady_clock::now();
    std::uint64_t counted = current;
    while (counted == current && std::chrono::steady_clock::now() - started < 15s) {
        rig.server->drain();
        std::this_thread::sleep_for(100ms);
        counted = space_bytes(rig);
    }
    return counted;
}

std::string letters(std::size_t size, std::uint64_t seed) {
    std::mt19937_64 generator(seed);
    std::string output;
    for (std::size_t index = 0; index < size; ++index) {
        output += static_cast<char>('a' + generator() % 26);
        if (index % 60 == 59) {
            output += '\n';
        }
    }
    return output;
}

std::uint64_t search_bytes(lake::test::FakeZettaObjects& zetta, const std::string& endpoint) {
    std::uint64_t total = 0;
    for (const auto& [key, bytes] : search_objects(zetta, endpoint)) {
        total += bytes.size();
    }
    return total;
}

// lake#381 review A: the index's room is the space's, as lake's quota check counts it.
TEST(WorkspaceStoreSearch, TheIndexBudgetCountsTheWholeSpaceAndWhatItWroteSinceTheLastReading) {
    MoveRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    const std::string endpoint_b = "http://" + rig.beta + ".test";
    ASSERT_EQ(rig.move(rig.alpha, "switch").first, 200U); // alpha holds about 2 MiB
    ASSERT_EQ(rig.move(rig.beta, "switch").first, 200U);
    const auto text = rig.create(rig.beta, "beta-text");
    ASSERT_TRUE(text);
    ASSERT_EQ(rig.commit(*text, "seed.txt", "seedterm381").first, 200U);
    ASSERT_EQ(searched(rig, "seedterm381&workspace_id=" + rig.beta)["value"]["items"].size(), 1U);
    // Lake's periodic work runs only when the test drives it, so no usage reading happens between
    // the steps below unless the test asks for one.
    const std::uint64_t total = next_space_bytes(rig);
    ASSERT_GT(total, 2'000'000U);

    // Room for beta's own bucket, none for the space: the commit lands, its index does not.
    ASSERT_TRUE(rig.store->set_storage_quota(rig.customer_id, total * 100 / 95));
    ASSERT_EQ(rig.commit(*text, "space.txt", "spaceterm381 " + letters(2000, 1)).first, 200U);
    EXPECT_TRUE(searched(rig, "spaceterm381&workspace_id=" + rig.beta)["value"]["items"].empty())
        << "the index counted only its own bucket";
    EXPECT_EQ(searched(rig, "seedterm381&workspace_id=" + rig.beta)["value"]["items"].size(), 1U);

    // Measure one commit's index bytes without a quota.
    ASSERT_TRUE(rig.store->set_storage_quota(rig.customer_id, 1ULL << 40));
    const std::uint64_t before = search_bytes(*rig.zetta, endpoint_b);
    ASSERT_EQ(rig.commit(*text, "one.txt", "oneterm381 " + letters(40'000, 2)).first, 200U);
    ASSERT_EQ(searched(rig, "oneterm381&workspace_id=" + rig.beta)["value"]["items"].size(), 1U);
    const std::uint64_t one = search_bytes(*rig.zetta, endpoint_b) - before;
    ASSERT_GT(one, 40'000U);
    // Once a usage reading includes it, leave room for one more such commit and a half.
    const std::uint64_t counted = next_space_bytes(rig);
    ASSERT_GT(counted, 2'000'000U);
    ASSERT_TRUE(rig.store->set_storage_quota(rig.customer_id, (counted + one * 3 / 2) * 10 / 9));
    ASSERT_EQ(rig.commit(*text, "two.txt", "twoterm381 " + letters(40'000, 3)).first, 200U);
    ASSERT_EQ(rig.commit(*text, "three.txt", "threeterm381 " + letters(40'000, 4)).first, 200U);
    EXPECT_EQ(searched(rig, "twoterm381&workspace_id=" + rig.beta)["value"]["items"].size(), 1U);
    EXPECT_TRUE(searched(rig, "threeterm381&workspace_id=" + rig.beta)["value"]["items"].empty())
        << "two commits within one usage reading each spent the whole room";
}

// lake#381 review B: rebuilds have their own worker.
TEST(WorkspaceStoreSearch, AUsageRefreshDoesNotWaitForARebuild) {
    MoveRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    const std::string endpoint_a = "http://" + rig.alpha + ".test";
    ASSERT_EQ(rig.move(rig.alpha, "switch").first, 200U);
    ASSERT_EQ(rig.move(rig.beta, "switch").first, 200U);
    const auto text = rig.create(rig.beta, "beta-text");
    ASSERT_TRUE(text);
    ASSERT_EQ(searched(rig, "readme&workspace_id=" + rig.alpha)["value"]["items"].size(), 1U);
    const std::uint64_t before = space_bytes_reaching(rig, 2'000'000);
    // Alpha's index is lost and its rebuild crawls: every index object takes 5 s.
    rig.options.search->set_memory_bound(1);
    ASSERT_EQ(searched(rig, "readme&workspace_id=" + rig.beta)["value"]["items"].size(), 1U);
    for (const auto& [key, bytes] : search_objects(*rig.zetta, endpoint_a)) {
        rig.zetta->erase_object(endpoint_a, key);
    }
    rig.zetta->hang(endpoint_a, 5s, "/search/");
    const auto [status, body] = rig.call("GET", "/v1/search?query=readme&workspace_id=" + rig.alpha);
    ASSERT_EQ(status, 200U) << body.dump();
    ASSERT_EQ(body["value"]["building_workspaces"], json::array({rig.alpha})) << body.dump();
    // A megabyte lands in beta; the usage refresh counts it while alpha's rebuild still runs.
    ASSERT_EQ(rig.commit(*text, "large.txt", letters(1'200'000, 5)).first, 200U);
    const std::uint64_t after = space_bytes_reaching(rig, before + 1'000'000);
    EXPECT_GE(after, before + 1'000'000) << "the usage refresh waited for the rebuild";
    rig.zetta->hang(endpoint_a, 0ms, "/search/");
}

// lake#381 review F: the Hugging Face upload's commit and index update are off the server thread.
TEST(WorkspaceStoreSearch, AHubUploadDoesNotHoldTheServerThread) {
    MoveRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    const std::string endpoint_a = "http://" + rig.alpha + ".test";
    ASSERT_EQ(rig.move(rig.alpha, "switch").first, 200U);
    const auto model = rig.create(rig.alpha, "hub-model");
    ASSERT_TRUE(model);
    ASSERT_EQ(rig.commit(*model, "README.md", "hubterm381").first, 200U);
    ASSERT_EQ(searched(rig, "hubterm381&workspace_id=" + rig.alpha)["value"]["items"].size(), 1U);
    // The upload's index update takes seconds.
    rig.zetta->hang(endpoint_a, 1500ms, "/search/");
    const std::string config = "{\"hidden\":8}";
    std::string encoded;
    {
        static constexpr std::string_view alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::size_t index = 0;
        for (; index + 2 < config.size(); index += 3) {
            const unsigned value = (static_cast<unsigned char>(config[index]) << 16U) |
                                   (static_cast<unsigned char>(config[index + 1]) << 8U) |
                                   static_cast<unsigned char>(config[index + 2]);
            for (int shift = 18; shift >= 0; shift -= 6) {
                encoded += alphabet[(value >> shift) & 63U];
            }
        }
        if (index < config.size()) {
            unsigned value = static_cast<unsigned char>(config[index]) << 16U;
            if (index + 1 < config.size()) {
                value |= static_cast<unsigned char>(config[index + 1]) << 8U;
            }
            encoded += alphabet[(value >> 18U) & 63U];
            encoded += alphabet[(value >> 12U) & 63U];
            encoded += index + 1 < config.size() ? alphabet[(value >> 6U) & 63U] : '=';
            encoded += '=';
        }
    }
    const std::string commit_body =
        json({{"key", "header"}, {"value", {{"summary", "upload"}}}}).dump() + "\n" +
        json({{"key", "file"},
              {"value", {{"path", "config.json"}, {"content", encoded}, {"encoding", "base64"}}}})
            .dump() +
        "\n";
    std::atomic<unsigned> uploaded{0};
    std::thread uploader([&] {
        const auto* bytes = reinterpret_cast<const std::byte*>(commit_body.data());
        const auto response = lake::test::http_request(
            {.url = "http://127.0.0.1:" + std::to_string(rig.server->port()),
             .method = "POST",
             .suffix = "/api/models/" + model->id + "/commit/main",
             .headers = {{"Authorization", "Bearer workspace-store-operator"},
                         {"Content-Type", "application/x-ndjson"}},
             .body = std::vector<std::byte>(bytes, bytes + commit_body.size())});
        uploaded.store(response ? response->status : 0U);
    });
    std::this_thread::sleep_for(500ms);
    const auto started = std::chrono::steady_clock::now();
    const auto [status, body] = rig.call("GET", "/v1/server_info");
    const auto took = std::chrono::steady_clock::now() - started;
    const bool still_uploading = uploaded.load() == 0;
    uploader.join();
    rig.zetta->hang(endpoint_a, 0ms, "/search/");
    EXPECT_EQ(status, 200U) << body.dump();
    EXPECT_TRUE(still_uploading) << "the upload finished before the check";
    EXPECT_LT(took, 1s) << "the upload held the server thread";
    EXPECT_EQ(uploaded.load(), 200U);
}

// lake#381 review nit: a write-once index object retried after Zetta took it is a success.
TEST(WorkspaceStoreSearch, AWriteOnceIndexPutRetriedWithTheSameContentSucceeds) {
    Rig rig;
    auto client = rig.client();
    ASSERT_TRUE(liquid::runtime::block_on(client->create_bucket(0)));
    lake::control::WorkspaceStoreCipher::Key key{};
    key.fill(7);
    const auto cipher = std::make_shared<const lake::control::WorkspaceStoreCipher>(rig.workspace, 1, key);
    const auto objects = lake::search::make_sealed_index_objects(client, cipher, 0, {});
    ASSERT_TRUE(objects->put("runs/one.json", "{\"a\":1}", true));
    EXPECT_TRUE(objects->put("runs/one.json", "{\"a\":1}", true)) << "a retry of the same put";
    const auto different = objects->put("runs/one.json", "{\"a\":2}", true);
    ASSERT_FALSE(different);
    EXPECT_EQ(objects->get("runs/one.json")->value(), "{\"a\":1}");
    EXPECT_FALSE(holds_text(rig.zetta->objects(rig.endpoint), "\"a\"")) << "sealed";
}

TEST(WorkspaceStoreSearch, OnceTheRepositoryStoreLentRepositoriesSearchNeverOpensAStoreItself) {
    const auto root = lake::test::fixture_path("workspace-store-search-" + lake::control::new_uuid_v4());
    std::filesystem::create_directories(root / "space" / "store");
    lake::search::Service service(root);
    service.set_repository_access([](const std::string&, const std::string&, bool)
                                      -> lake::search::Result<lake::search::BorrowedRepository> {
        return std::unexpected(lake::search::Error{"store_unavailable", "lent store refused"});
    });
    // The repository store closes (a server stopping): search refuses rather than open the space
    // store a second time beside its writer.
    service.set_repository_access({});
    const auto reindexed = service.reindex_artifacts("space", "repository");
    ASSERT_FALSE(reindexed);
    EXPECT_EQ(reindexed.error().code, "store_unavailable");
    EXPECT_FALSE(std::filesystem::exists(root / "space" / "store" / "LIQUID_STORE"))
        << "search opened the store itself";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST(WorkspaceStoreMove, ASearchWaitingOnOneSpacesStoreDoesNotHoldSearchInAnother) {
    MoveRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    const std::string endpoint_a = "http://" + rig.alpha + ".test";
    const auto history = [&](const std::string& customer, const lake::control::Repository& repository) {
        return rig.call("GET", "/v1/search?query=readme&customer=" + customer + "&repository=" +
                                   repository.id + "&history=true");
    };
    // Alpha's copy holds the first space's store for seconds against a slow Zetta.
    rig.zetta->hang(endpoint_a, 60ms);
    std::atomic<bool> copy_done{false};
    std::thread copier([&] {
        static_cast<void>(rig.move(rig.alpha, "copy"));
        copy_done.store(true);
    });
    std::this_thread::sleep_for(300ms);
    // A search of beta's repository waits for the first space's store...
    std::thread waiting([&] { static_cast<void>(history(rig.customer_id, rig.repo_b)); });
    std::this_thread::sleep_for(200ms);
    // ...while a search in the other space is answered at once.
    const bool copying = !copy_done.load();
    const auto started = std::chrono::steady_clock::now();
    const auto [status, body] = history(rig.other_customer_id, rig.repo_c);
    const auto took = std::chrono::steady_clock::now() - started;
    waiting.join();
    copier.join();
    rig.zetta->hang(endpoint_a, 0ms);
    EXPECT_TRUE(copying) << "the copy finished before the check";
    EXPECT_EQ(status, 200U) << body.dump();
    EXPECT_LT(took, 1s) << "search in another space waited for the first space's store";
}

TEST(WorkspaceStoreMove, AStoppedSwitchedWorkspaceDoesNotRefuseItsSpacesWrites) {
    MoveRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    ASSERT_EQ(rig.move(rig.alpha, "copy").first, 200U);
    ASSERT_EQ(rig.move(rig.alpha, "switch").first, 200U);
    ASSERT_TRUE(rig.store->update_workspace(rig.alpha, {.desired = "stopped"}));
    ASSERT_NO_FATAL_FAILURE(rig.restart());
    const auto answer = rig.commit(rig.repo_b, "beta.txt", "a write while alpha is stopped");
    EXPECT_EQ(answer.first, 200U) << answer.second.dump();
}

TEST(WorkspaceStoreMove, ADeletedWorkspaceIsNeverReopenedAndADeleteWaitsForAnOpen) {
    MoveRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    const std::string endpoint_a = "http://" + rig.alpha + ".test";
    ASSERT_EQ(rig.move(rig.alpha, "copy").first, 200U);
    ASSERT_EQ(rig.move(rig.alpha, "switch").first, 200U);
    ASSERT_NO_FATAL_FAILURE(rig.restart());
    // Another process holds alpha's lease; a request starts opening alpha's store and waits for it.
    rig.zetta->take_over(endpoint_a, "predecessor");
    std::thread opener([&] {
        static_cast<void>(rig.call("GET", "/v1/repository_tree?id=" + rig.repo_a.id + "&ref=main"));
    });
    std::this_thread::sleep_for(300ms);
    std::pair<unsigned, json> deleted;
    std::thread deleter([&] {
        deleted = rig.call("POST", "/v1/delete_workspace",
                           {{"customer_id", rig.customer_id}, {"workspace_id", rig.alpha},
                            {"confirm", "alpha"}});
    });
    std::this_thread::sleep_for(300ms);
    rig.zetta->release(endpoint_a); // the open takes the lease, then the deletion closes it
    opener.join();
    deleter.join();
    ASSERT_EQ(deleted.first, 200U) << deleted.second.dump();
    EXPECT_TRUE(rig.zetta->lease(endpoint_a).holder.empty()) << "the deletion released the lease";
    // A deleted workspace's store is not opened again, and its repository is not served from the
    // space store either.
    const auto fence = rig.zetta->lease(endpoint_a).fence;
    const auto [after_status, after] =
        rig.call("GET", "/v1/repository_tree?id=" + rig.repo_a.id + "&ref=main");
    EXPECT_NE(after_status, 200U) << after.dump();
    EXPECT_TRUE(rig.zetta->lease(endpoint_a).holder.empty());
    EXPECT_EQ(rig.zetta->lease(endpoint_a).fence, fence);
}

// ---- lake#527: a commit's store requests in its Server-Timing and the process metrics ----------

// One request on its own connection; the whole response, headers included.
std::string raw_exchange(int port, const std::string& method, const std::string& path, const std::string& body) {
    const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(socket_fd);
        return {};
    }
    const std::string request = method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                                "Authorization: Bearer workspace-store-operator\r\n"
                                "Content-Type: application/json\r\nConnection: close\r\n"
                                "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    std::size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t count = ::send(socket_fd, request.data() + sent, request.size() - sent, 0);
        if (count <= 0) {
            break;
        }
        sent += static_cast<std::size_t>(count);
    }
    std::string response;
    char buffer[4096];
    for (;;) {
        const ssize_t count = ::recv(socket_fd, buffer, sizeof(buffer), 0);
        if (count <= 0) {
            break;
        }
        response.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(socket_fd);
    return response;
}

std::string header_of(const std::string& response, std::string_view name) {
    std::string lowered = response.substr(0, response.find("\r\n\r\n"));
    std::ranges::transform(lowered, lowered.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    const std::string prefix = "\r\n" + std::string(name) + ": ";
    const auto start = lowered.find(prefix);
    if (start == std::string::npos) {
        return {};
    }
    const auto end = lowered.find("\r\n", start + prefix.size());
    return response.substr(start + prefix.size(), end - start - prefix.size());
}

std::uint64_t sum_of(const std::map<std::string, std::uint64_t>& counts) {
    std::uint64_t total = 0;
    for (const auto& [ignored, count] : counts) {
        total += count;
    }
    return total;
}

// A commit_repository_files into a switched workspace names its workspace store requests in
// Server-Timing (`store`, then one entry per kind), the count is every request the workspace
// Zetta received for it (nothing else on lake's commit path reaches the store), and the process
// metrics count them by kind and outcome with no workspace label.
TEST(WorkspaceStoreMove, ACommitNamesItsStoreRequestsInServerTimingAndMetrics) {
    MoveRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    ASSERT_EQ(rig.move(rig.alpha, "switch").first, 200U);
    ASSERT_TRUE(written(rig.commit(rig.repo_a, "warm.txt", "the store is open after this")));
    const json body{{"id", rig.repo_a.id},
                    {"revision", "main"},
                    {"message", "One file"},
                    {"operations", json::array({{{"type", "file"}, {"path", "one.txt"}, {"content", "b25lCg=="}}})}};
    const std::uint64_t before = sum_of(rig.zetta->requests_by_method());
    const std::size_t applied_before = rig.zetta->applied().size();
    const std::string alpha_endpoint = "http://" + rig.alpha + ".test";
    const std::uint64_t durable_before = rig.zetta->durable_requests(alpha_endpoint);
    const std::string response = raw_exchange(rig.server->port(), "POST", "/v1/commit_repository_files", body.dump());
    const std::uint64_t received = sum_of(rig.zetta->requests_by_method()) - before;
    const std::uint64_t durable = rig.zetta->durable_requests(alpha_endpoint) - durable_before;
    ASSERT_TRUE(response.starts_with("HTTP/1.1 200")) << response;
    const std::string timing = header_of(response, "server-timing");
    ASSERT_FALSE(timing.empty()) << response;
    const std::string expected = "store;dur=";
    const auto store = timing.find(expected);
    ASSERT_NE(store, std::string::npos) << timing;
    const auto described = timing.find(";desc=\"", store);
    ASSERT_NE(described, std::string::npos) << timing;
    const std::uint64_t named = std::stoull(timing.substr(described + 7));
    EXPECT_GT(named, 0U) << timing;
    EXPECT_EQ(named, received) << timing;
    EXPECT_NE(timing.find("store_put;dur="), std::string::npos) << timing;
    // Liquid's writes are sent without durability and made durable by one :barrier before lake
    // answers (lake#535); since lake#381 the search index's own writes for the commit go to the
    // same bucket and end with its own :barrier. Nothing else asks Zetta to fsync.
    EXPECT_GE(durable, 1U) << timing;
    EXPECT_LE(durable, 2U) << timing;
    std::size_t index_writes = 0;
    std::size_t store_writes = 0;
    const auto applied = rig.zetta->applied();
    for (std::size_t index = applied_before; index < applied.size(); ++index) {
        const std::string key = applied[index].substr(applied[index].find(' ') + 1);
        (key.starts_with(lake::search::workspace_index_prefix) ? index_writes : store_writes) += 1;
    }
    std::printf("commit writes: %zu to the repository store, %zu to the search index; %llu durable requests\n",
                store_writes, index_writes, static_cast<unsigned long long>(durable));
    std::printf("commit_repository_files Server-Timing: %s\n", timing.c_str());

    const std::string metrics = raw_exchange(rig.server->port(), "GET", "/metrics", "");
    EXPECT_NE(metrics.find("lake_workspace_store_requests_total{kind=\"put\",outcome=\"ok\"}"),
              std::string::npos);
    EXPECT_NE(metrics.find("lake_workspace_store_request_duration_seconds_bucket{kind=\"put\""),
              std::string::npos);
    EXPECT_EQ(metrics.find("lake_workspace_store_requests_total{kind=\"put\",outcome=\"ok\",workspace"),
              std::string::npos);
}

// ---- against a real Zetta --------------------------------------------------------------------

// ---- remove_space_store: the space's old store leaves lake's volume ----------------------

std::pair<unsigned, json> remove_space(MoveRig& rig, const std::string& customer_id, bool dry_run) {
    return rig.call("POST", "/v1/remove_space_store", {{"customer_id", customer_id}, {"dry_run", dry_run}});
}

// The capability refuses while a workspace is not switched (dry run included), reports the
// directories and their bytes on a dry run without touching them, removes exactly the space's
// store and repository directories, leaves the instance's directory and other spaces alone, keeps
// the switched workspaces answering (also after a restart), and is audited centrally. A workspace
// created afterwards and not switched gets a new, empty space store on first use, and the removal
// is refused again until it is switched.
TEST(WorkspaceStoreRemoval, RemovesOnlyTheSpacesStoreOnceEveryWorkspaceIsSwitched) {
    MoveRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    const auto space = rig.root / rig.customer_id;
    // The instance's own directory (the registry, the log) sits beside the spaces.
    const auto instance = rig.root / rig.options.node_name;
    std::filesystem::create_directories(instance / "registry");
    std::ofstream(instance / "registry" / "blob") << "instance registry bytes";
    std::filesystem::create_directories(instance / "log");
    std::ofstream(instance / "log" / "entries") << "instance log bytes";
    const auto instance_before = files_under(instance);
    const auto other_before = files_under(rig.root / rig.other_customer_id);
    EXPECT_EQ(rig.tree(rig.repo_a), (std::vector<std::string>{"README.md", "data", "data/blob.bin"}));

    // Not a space, the instance's directory, a path and an unknown space are refused.
    for (const std::string& bad : {std::string("lake"), std::string("../" + rig.customer_id),
                                   std::string("workspaces")}) {
        const auto [status, body] = remove_space(rig, bad, true);
        EXPECT_EQ(status, 400U) << bad << " " << body.dump();
        EXPECT_EQ(body["error"]["code"], "invalid_input") << body.dump();
    }
    EXPECT_EQ(remove_space(rig, lake::control::new_uuid_v4(), true).first, 404U);

    // Refused, dry run included, while any workspace of the space is not switched.
    for (const bool dry_run : {true, false}) {
        const auto [status, body] = remove_space(rig, rig.customer_id, dry_run);
        EXPECT_EQ(status, 409U) << body.dump();
        EXPECT_EQ(body["error"]["code"], "conflict");
        EXPECT_NE(body["error"]["message"].get<std::string>().find(rig.alpha), std::string::npos);
    }
    ASSERT_EQ(rig.move(rig.alpha, "switch").first, 200U);
    {
        const auto [status, body] = remove_space(rig, rig.customer_id, false);
        EXPECT_EQ(status, 409U) << "beta is not switched yet: " << body.dump();
        EXPECT_NE(body["error"]["message"].get<std::string>().find(rig.beta), std::string::npos);
    }
    EXPECT_TRUE(std::filesystem::exists(space / "store"));
    ASSERT_EQ(rig.move(rig.beta, "switch").first, 200U);

    // A dry run reports both directories and their bytes, and changes nothing.
    const auto store_before = files_under(space / "store");
    std::uintmax_t expected = 0;
    for (const auto& directory : {space / "store", space / "repositories"}) {
        for (const auto& [name, size] : files_under(directory)) {
            expected += size;
        }
    }
    const auto [dry_status, dry] = remove_space(rig, rig.customer_id, true);
    ASSERT_EQ(dry_status, 200U) << dry.dump();
    EXPECT_TRUE(dry["value"]["dry_run"].get<bool>());
    EXPECT_FALSE(dry["value"]["removed"].get<bool>());
    EXPECT_EQ(dry["value"]["bytes"].get<std::uintmax_t>(), expected);
    EXPECT_GT(expected, 2'000'000U);
    ASSERT_EQ(dry["value"]["directories"].size(), 2U);
    EXPECT_EQ(dry["value"]["directories"][0]["path"], (space / "store").string());
    EXPECT_EQ(dry["value"]["directories"][1]["path"], (space / "repositories").string());
    EXPECT_TRUE(dry["value"]["directories"][0]["exists"].get<bool>());
    EXPECT_EQ(files_under(space / "store"), store_before) << "a dry run removes nothing";

    // The removal: exactly the two directories go.
    const auto [removed_status, removed] = remove_space(rig, rig.customer_id, false);
    ASSERT_EQ(removed_status, 200U) << removed.dump();
    EXPECT_TRUE(removed["value"]["removed"].get<bool>());
    EXPECT_EQ(removed["value"]["bytes"].get<std::uintmax_t>(), expected);
    EXPECT_FALSE(std::filesystem::exists(space / "store"));
    EXPECT_FALSE(std::filesystem::exists(space / "repositories"));
    EXPECT_EQ(files_under(instance), instance_before) << "the instance's directory is untouched";
    EXPECT_EQ(files_under(rig.root / rig.other_customer_id), other_before) << "another space is untouched";

    // The switched workspaces still answer, read and write.
    EXPECT_EQ(rig.tree(rig.repo_a), (std::vector<std::string>{"README.md", "data", "data/blob.bin"}));
    EXPECT_EQ(rig.tree(rig.repo_b), (std::vector<std::string>{"README.md"}));
    EXPECT_EQ(rig.tree(rig.repo_c), (std::vector<std::string>{"README.md"}));
    if (written(rig.commit(rig.repo_a, "after-removal.txt", "written after the removal"))) {
        EXPECT_EQ(rig.tree(rig.repo_a), (std::vector<std::string>{"README.md", "after-removal.txt", "data",
                                                                  "data/blob.bin"}));
    }
    EXPECT_FALSE(std::filesystem::exists(space / "store")) << "a switched workspace never reopens it";
    // Again: nothing is left, nothing is removed.
    const auto [again_status, again] = remove_space(rig, rig.customer_id, false);
    ASSERT_EQ(again_status, 200U) << again.dump();
    EXPECT_EQ(again["value"]["bytes"], 0U);
    EXPECT_FALSE(again["value"]["directories"][0]["exists"].get<bool>());
    // A new process on the same volume serves the switched workspaces as before.
    ASSERT_NO_FATAL_FAILURE(rig.restart());
    EXPECT_EQ(rig.tree(rig.repo_b), (std::vector<std::string>{"README.md"}));

    // Every call is one central audit event.
    const auto events = lake::test::audit_events(*rig.store, {.limit = 500});
    ASSERT_TRUE(events) << events.error().message;
    int successes = 0;
    int refusals = 0;
    for (const auto& event : *events) {
        if (event.draft.action == "remove_space_store") {
            successes += event.draft.outcome == "success";
            refusals += event.draft.outcome != "success";
        }
    }
    EXPECT_EQ(successes, 3);
    EXPECT_GE(refusals, 3);

    // A workspace created afterwards and not switched yet gets a new, empty space store on first
    // use, and the removal is refused again until it is switched.
    const std::string delta = rig.workspaces->create(rig.customer_id, "delta");
    rig.zetta->add_node("http://" + delta + ".test", "token-" + delta);
    const auto fresh = rig.create(delta, "delta-source");
    ASSERT_TRUE(fresh);
    if (written(rig.commit(*fresh, "delta.txt", "in a new space store"))) {
        EXPECT_EQ(rig.tree(*fresh), (std::vector<std::string>{"delta.txt"}));
    }
    EXPECT_TRUE(std::filesystem::exists(space / "store"));
    EXPECT_EQ(remove_space(rig, rig.customer_id, true).first, 409U);
    ASSERT_EQ(rig.move(delta, "copy").first, 200U);
    ASSERT_EQ(rig.move(delta, "switch").first, 200U);
    ASSERT_EQ(remove_space(rig, rig.customer_id, false).first, 200U);
    EXPECT_FALSE(std::filesystem::exists(space / "store"));
    EXPECT_EQ(rig.tree(*fresh), (std::vector<std::string>{"delta.txt"}));
    EXPECT_EQ(files_under(instance), instance_before);
}

// A symbolic link where the space's directory, or its store, should be would take the removal
// outside the store root: both are refused, dry run included, and nothing behind them is touched.
TEST(WorkspaceStoreRemoval, ALinkedSpaceOrStoreDirectoryIsRefused) {
    MoveRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    ASSERT_EQ(rig.move(rig.alpha, "switch").first, 200U);
    ASSERT_EQ(rig.move(rig.beta, "switch").first, 200U);
    const auto space = rig.root / rig.customer_id;
    const auto elsewhere = rig.root / "elsewhere";
    std::filesystem::create_directories(elsewhere / "store");
    std::ofstream(elsewhere / "store" / "keep") << "not the space's";

    // <space>/store is a link.
    const auto real_store = rig.root / "real-store";
    std::filesystem::rename(space / "store", real_store);
    std::filesystem::create_directory_symlink(elsewhere / "store", space / "store");
    for (const bool dry_run : {true, false}) {
        const auto [status, body] = remove_space(rig, rig.customer_id, dry_run);
        EXPECT_EQ(status, 409U) << body.dump();
        EXPECT_EQ(body["error"]["code"], "conflict") << body.dump();
    }
    EXPECT_TRUE(std::filesystem::exists(elsewhere / "store" / "keep"));
    std::filesystem::remove(space / "store");
    std::filesystem::rename(real_store, space / "store");

    // <space> itself is a link.
    const auto real_space = rig.root / "real-space";
    std::filesystem::rename(space, real_space);
    std::filesystem::create_directory_symlink(elsewhere, space);
    for (const bool dry_run : {true, false}) {
        const auto [status, body] = remove_space(rig, rig.customer_id, dry_run);
        EXPECT_EQ(status, 409U) << body.dump();
        EXPECT_EQ(body["error"]["code"], "conflict") << body.dump();
        EXPECT_NE(body["error"]["message"].get<std::string>().find("not a directory"),
                  std::string::npos)
            << body.dump();
    }
    EXPECT_TRUE(std::filesystem::exists(elsewhere / "store" / "keep"));
    EXPECT_TRUE(std::filesystem::exists(real_space / "store"));
    std::filesystem::remove(space);
    std::filesystem::rename(real_space, space);
    // Put back, the removal goes ahead.
    EXPECT_EQ(remove_space(rig, rig.customer_id, false).first, 200U);
    EXPECT_FALSE(std::filesystem::exists(space / "store"));
    EXPECT_TRUE(std::filesystem::exists(elsewhere / "store" / "keep"));
}

// A live repository of a deleted workspace (never moved) keeps the space's store, even when every
// live workspace is switched; and only the operator removes a space's store.
TEST(WorkspaceStoreRemoval, ALiveRepositoryOfADeletedWorkspaceOrAPersonIsRefused) {
    MoveRig rig;
    ASSERT_NO_FATAL_FAILURE(rig.SetUp());
    const std::string gone = rig.workspaces->create(rig.customer_id, "gone");
    const auto stranded = rig.store->create_repository({rig.customer_id, "stranded", "", "private", gone});
    ASSERT_TRUE(stranded) << stranded.error().message;
    ASSERT_TRUE(rig.store->update_workspace(gone, {.desired = "deleted"}));
    ASSERT_EQ(rig.move(rig.alpha, "switch").first, 200U);
    ASSERT_EQ(rig.move(rig.beta, "switch").first, 200U);
    for (const bool dry_run : {true, false}) {
        const auto [status, body] = remove_space(rig, rig.customer_id, dry_run);
        EXPECT_EQ(status, 409U) << body.dump();
        EXPECT_NE(body["error"]["message"].get<std::string>().find(stranded->id), std::string::npos)
            << body.dump();
    }
    EXPECT_TRUE(std::filesystem::exists(rig.root / rig.customer_id / "store"));

    // Without the operator bearer, and as the space's owner through the registry.
    const auto unauthenticated = lake::test::http_request(
        {.url = "http://127.0.0.1:" + std::to_string(rig.server->port()),
         .method = "POST",
         .suffix = "/v1/remove_space_store",
         .headers = {{"Content-Type", "application/json"}},
         .body = bytes_of(json{{"customer_id", rig.customer_id}}.dump())});
    ASSERT_TRUE(unauthenticated);
    EXPECT_EQ(unauthenticated->status, 401U);
    lake::api::Registry registry;
    lake::api::register_workspace_store_capabilities(registry);
    lake::api::ApiContext context;
    context.store = rig.store.get();
    context.authorized = true;
    context.acting_identity = "taap:workspace-store";
    context.operator_credential = false;
    context.instance_kek = std::string(kek);
    const auto person = registry.call("remove_space_store", {{"customer_id", rig.customer_id}}, context);
    ASSERT_FALSE(person.ok);
    EXPECT_EQ(person.error.code, "unauthorized");
    EXPECT_TRUE(std::filesystem::exists(rig.root / rig.customer_id / "store"));
}

// A repository of the space without a workspace (left so by migration 0051 until lake#513) keeps
// the space's store although its only workspace is switched. PostgreSQL, where such a row exists.
TEST(WorkspaceStoreRemoval, AnUnassignedRepositoryIsRefused) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || *uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    auto store = lake::control::make_postgres_store(uri, std::string(kek));
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const auto user = store->create_user({"taap:space-removal-" + suffix, "", "Removal"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = store->create_customer({"individual", user->id, "", "space-removal-" + suffix});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto workspace = store->create_workspace({customer->id, "main", "dev", ""});
    ASSERT_TRUE(workspace) << workspace.error().message;
    const auto loose = store->create_repository({customer->id, "loose", "", "private", workspace->id});
    ASSERT_TRUE(loose) << loose.error().message;
    ASSERT_TRUE(store->mark_workspace_store_moved(workspace->id));
    {
        // What a pre-0051 row looks like; the routing trigger is off only inside this transaction.
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        tx.exec("ALTER TABLE repositories DISABLE TRIGGER repository_workspace_routing");
        tx.exec_params("UPDATE repositories SET workspace_id = NULL WHERE id = $1::uuid", loose->id);
        tx.exec("ALTER TABLE repositories ENABLE TRIGGER repository_workspace_routing");
        tx.commit();
    }
    lake::api::Registry registry;
    lake::api::register_workspace_store_capabilities(registry);
    lake::api::ApiContext context;
    context.store = store.get();
    context.authorized = true;
    context.acting_identity = "admin";
    context.operator_credential = true;
    context.instance_kek = std::string(kek);
    for (const bool dry_run : {true, false}) {
        const auto refused = registry.call("remove_space_store",
                                           {{"customer_id", customer->id}, {"dry_run", dry_run}}, context);
        ASSERT_FALSE(refused.ok);
        EXPECT_EQ(refused.error.code, "conflict");
        EXPECT_NE(refused.error.message.find(loose->id), std::string::npos) << refused.error.message;
        EXPECT_NE(refused.error.message.find("has no workspace"), std::string::npos);
    }
}

struct RealZetta {
    pid_t pid = -1;
    std::filesystem::path directory;
    std::string url;
    std::string token = "lake-380-zetta-admin";
    int port = 0;
    bool installed = false;

    bool start() {
        const char* binary = std::getenv("LAKE_TEST_ZETTA_BINARY");
        if (binary == nullptr || *binary == '\0') {
            return false;
        }
        directory = lake::test::fixture_path("workspace-store-zetta-" + lake::control::new_uuid_v4());
        std::filesystem::create_directories(directory);
        std::mt19937 generator(std::random_device{}());
        const int mcp = 20000 + static_cast<int>(generator() % 20000);
        const int cluster = mcp + 1;
        port = mcp;
        url = "http://127.0.0.1:" + std::to_string(mcp);
        const std::string data = (directory / "data").string();
        const std::string log = (directory / "zetta.log").string();
        std::vector<std::string> arguments = {binary, "--headless", "--no-preflight", "--data-dir", data,
                                              "--admin-token", token, "--mcp-listen",
                                              "127.0.0.1:" + std::to_string(mcp), "--listen",
                                              "127.0.0.1:" + std::to_string(cluster)};
        std::vector<char*> argv;
        for (std::string& argument : arguments) {
            argv.push_back(argument.data());
        }
        argv.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, 1, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        posix_spawn_file_actions_adddup2(&actions, 1, 2);
        const int spawned = posix_spawn(&pid, binary, &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        if (spawned != 0) {
            return false;
        }
        lake::control::install_zetta_control_transport();
        installed = true;
        auto client = std::make_shared<ZettaObjectClient>(lake::control::make_zetta_object_transport(),
                                                          ObjectEndpoint{url, token});
        for (int attempt = 0; attempt < 300; ++attempt) {
            if (liquid::runtime::block_on(client->create_bucket(0))) {
                return true;
            }
            std::this_thread::sleep_for(100ms);
        }
        return false;
    }

    ~RealZetta() {
        if (installed) {
            lake::control::shutdown_zetta_control_transport();
        }
        if (pid > 0) {
            kill(pid, SIGTERM);
            int status = 0;
            waitpid(pid, &status, 0);
        }
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
};

double milliseconds_since(std::chrono::steady_clock::time_point started) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
}

struct Timing {
    double put_ms = 0;
    double get_ms = 0;
    double large_put_ms = 0;
    double large_get_ms = 0;
    double small_writes_ms = 0;
    double transfer_ms = 0;
};

// The same work on a store: 64 MiB as 64 objects of 1 MiB put then read, one 100 MiB object put
// then read (a Drive upload and download), 200 small objects each put on its own (a push of small
// files, each publication with its barriers), and a repository with history copied in with
// liquid#328's transfer.
Timing measure(liquid::store& store, liquid::store* transfer_source,
               const std::filesystem::path& transfer_identifier) {
    Timing timing;
    std::vector<std::pair<liquid::digest, std::size_t>> ids;
    auto started = std::chrono::steady_clock::now();
    for (std::uint64_t index = 0; index < 64; ++index) {
        auto id = liquid::runtime::block_on(store.put(random_bytes(1024 * 1024, 900 + index)));
        EXPECT_TRUE(id) << id.error().message;
        if (!id) {
            return timing;
        }
        ids.emplace_back(*id, 1024 * 1024);
    }
    timing.put_ms = milliseconds_since(started);
    started = std::chrono::steady_clock::now();
    for (const auto& [id, size] : ids) {
        auto bytes = liquid::runtime::block_on(store.get(id));
        EXPECT_TRUE(bytes && bytes->size() == size);
    }
    timing.get_ms = milliseconds_since(started);
    started = std::chrono::steady_clock::now();
    auto large = liquid::runtime::block_on(store.put(random_bytes(100 * 1024 * 1024, 999)));
    EXPECT_TRUE(large);
    timing.large_put_ms = milliseconds_since(started);
    started = std::chrono::steady_clock::now();
    auto large_back = liquid::runtime::block_on(store.get(*large));
    EXPECT_TRUE(large_back && large_back->size() == 100U * 1024U * 1024U);
    timing.large_get_ms = milliseconds_since(started);
    started = std::chrono::steady_clock::now();
    for (std::uint64_t index = 0; index < 200; ++index) {
        EXPECT_TRUE(liquid::runtime::block_on(store.put(random_bytes(4096, 2000 + index))));
    }
    timing.small_writes_ms = milliseconds_since(started);
    if (transfer_source != nullptr) {
        started = std::chrono::steady_clock::now();
        auto copied = liquid::runtime::block_on(
            liquid::repository::transfer(*transfer_source, store, transfer_identifier));
        EXPECT_TRUE(copied && copied->complete) << (copied ? "" : copied.error().message);
        timing.transfer_ms = milliseconds_since(started);
    }
    return timing;
}

TEST(WorkspaceStoreZetta, TheBackendOnARealZettaHoldsOnlyCiphertextAndItsLatencyIsMeasured) {
    RealZetta zetta;
    if (!zetta.start()) {
        GTEST_SKIP() << "set LAKE_TEST_ZETTA_BINARY to a zetta built from origin/dev";
    }
    const std::string workspace = lake::control::new_uuid_v4();
    auto client = std::make_shared<ZettaObjectClient>(lake::control::make_zetta_object_transport(),
                                                      ObjectEndpoint{zetta.url, zetta.token});
    ASSERT_TRUE(liquid::runtime::block_on(client->create_bucket(0)));
    const auto open_on_zetta = [&](std::shared_ptr<ZettaStoreOwner>& owner, std::chrono::milliseconds wait) {
        owner = std::make_shared<ZettaStoreOwner>(client, 30s);
        liquid::store_options options;
        options.backend = std::make_shared<ZettaObjectBackend>(
            client, ZettaObjectBackendOptions{.workspace_id = workspace,
                                              .instance_kek = std::string(kek),
                                              .create_key = true});
        options.owner = owner;
        options.owner_wait = wait;
        return liquid::runtime::block_on(liquid::store::open(zetta.directory / "unused", std::move(options)));
    };

    // A disk store with a repository to copy, for the transfer timing on both backends.
    const auto disk_root = zetta.directory / "disk";
    auto source = liquid::runtime::block_on(liquid::store::open(disk_root / "source"));
    ASSERT_TRUE(source) << source.error().message;
    const auto identifier = disk_root / "repositories" / "measured";
    {
        liquid::repository_options options;
        options.store = &*source;
        auto repository = liquid::runtime::block_on(liquid::repository::create(identifier, options));
        ASSERT_TRUE(repository);
        for (int change = 0; change < 20; ++change) {
            for (int file = 0; file < 10; ++file) {
                ASSERT_TRUE(liquid::runtime::block_on(repository->write(
                    "src/file-" + std::to_string(file) + ".txt",
                    random_bytes(16 * 1024, static_cast<std::uint64_t>(change * 100 + file)))));
            }
            auto cut = liquid::runtime::block_on(repository->cut("change " + std::to_string(change)));
            ASSERT_TRUE(cut);
            ASSERT_TRUE(liquid::runtime::block_on(repository->set_ref("main", *cut)));
        }
    }

    std::shared_ptr<ZettaStoreOwner> owner;
    auto store = open_on_zetta(owner, 0ms);
    ASSERT_TRUE(store) << store.error().message;
    const auto marked = with_marker(random_bytes(200000, 1), 4096);
    auto marked_id = liquid::runtime::block_on(store->put(marked));
    ASSERT_TRUE(marked_id);
    // Each object one put: what a push of new files does.
    ASSERT_TRUE(liquid::runtime::block_on(store->close()));
    std::shared_ptr<ZettaStoreOwner> measuring_owner;
    auto measuring = open_on_zetta(measuring_owner, 5s);
    ASSERT_TRUE(measuring) << measuring.error().message;
    const Timing on_zetta = measure(*measuring, &*source, identifier);
    static_cast<void>(liquid::runtime::block_on(measuring->close()));
    const auto counters = client->counters();

    // Reopened by a successor: every object is there, and no stored byte is plaintext.
    std::shared_ptr<ZettaStoreOwner> successor_owner;
    auto successor = open_on_zetta(successor_owner, 5s);
    ASSERT_TRUE(successor) << successor.error().message;
    auto back = liquid::runtime::block_on(successor->get(*marked_id));
    ASSERT_TRUE(back);
    EXPECT_EQ(*back, marked);
    ASSERT_TRUE(liquid::runtime::block_on(successor->close()));
    auto listed = liquid::runtime::block_on(client->list(""));
    ASSERT_TRUE(listed);
    for (const auto& object : *listed) {
        auto bytes = liquid::runtime::block_on(client->get_all(object.key));
        ASSERT_TRUE(bytes) << object.key;
        EXPECT_EQ(bytes->find(marker), std::string::npos) << object.key;
    }

    auto disk = liquid::runtime::block_on(liquid::store::open(disk_root / "measured"));
    ASSERT_TRUE(disk);
    const Timing on_disk = measure(*disk, &*source, identifier);
    std::printf("\nlake#380 latency, workspace store on a real Zetta (loopback) beside the disk backend\n");
    std::printf("| Case | Disk | Zetta |\n|---|---:|---:|\n");
    std::printf("| 64 x 1 MiB put | %.0f ms | %.0f ms |\n", on_disk.put_ms, on_zetta.put_ms);
    std::printf("| 64 x 1 MiB get | %.0f ms | %.0f ms |\n", on_disk.get_ms, on_zetta.get_ms);
    std::printf("| 100 MiB put (Drive upload) | %.0f ms | %.0f ms |\n", on_disk.large_put_ms, on_zetta.large_put_ms);
    std::printf("| 100 MiB get (Drive download) | %.0f ms | %.0f ms |\n", on_disk.large_get_ms, on_zetta.large_get_ms);
    std::printf("| 200 x 4 KiB puts | %.0f ms | %.0f ms |\n", on_disk.small_writes_ms, on_zetta.small_writes_ms);
    std::printf("| transfer of a 20-change repository | %.0f ms | %.0f ms |\n", on_disk.transfer_ms, on_zetta.transfer_ms);
    std::printf("Zetta requests: %llu (%.2f ms mean), %llu MiB sent, %llu MiB received, window hits %llu / misses %llu\n",
                static_cast<unsigned long long>(counters->requests.load()),
                counters->requests.load() == 0
                    ? 0.0
                    : static_cast<double>(counters->request_nanoseconds.load()) / 1e6 /
                          static_cast<double>(counters->requests.load()),
                static_cast<unsigned long long>(counters->bytes_sent.load() >> 20U),
                static_cast<unsigned long long>(counters->bytes_received.load() >> 20U),
                static_cast<unsigned long long>(counters->window_hits.load()),
                static_cast<unsigned long long>(counters->window_misses.load()));
    std::printf("by kind: %llu puts, %llu appends, %llu gets, %llu lists, %llu removes, %llu barriers\n",
                static_cast<unsigned long long>(counters->puts.load()),
                static_cast<unsigned long long>(counters->appends.load()),
                static_cast<unsigned long long>(counters->gets.load()),
                static_cast<unsigned long long>(counters->lists.load()),
                static_cast<unsigned long long>(counters->removes.load()),
                static_cast<unsigned long long>(counters->barriers.load()));
}

// lake#381 on a real Zetta: a workspace's index written through its store's backend is sealed in
// the bucket, reloads in a new process, and is rebuilt once wiped.
TEST(WorkspaceStoreZetta, TheSearchIndexOnARealZettaIsSealedReloadsAndIsRebuilt) {
    RealZetta zetta;
    if (!zetta.start()) {
        GTEST_SKIP() << "set LAKE_TEST_ZETTA_BINARY to a zetta built from origin/dev";
    }
    const std::string workspace = lake::control::new_uuid_v4();
    auto client = std::make_shared<ZettaObjectClient>(lake::control::make_zetta_object_transport(),
                                                      ObjectEndpoint{zetta.url, zetta.token});
    ASSERT_TRUE(liquid::runtime::block_on(client->create_bucket(0)));
    auto owner = std::make_shared<ZettaStoreOwner>(client, 30s);
    auto backend = std::make_shared<ZettaObjectBackend>(
        client, ZettaObjectBackendOptions{.workspace_id = workspace,
                                          .instance_kek = std::string(kek),
                                          .create_key = true});
    liquid::store_options options;
    options.backend = backend;
    options.owner = owner;
    auto store = liquid::runtime::block_on(liquid::store::open(zetta.directory / "unused", std::move(options)));
    ASSERT_TRUE(store) << store.error().message;
    const std::filesystem::path identifier = zetta.directory / "repositories" / "searched";
    liquid::repository_options repository_options;
    repository_options.store = &*store;
    auto repository = liquid::runtime::block_on(liquid::repository::create(identifier, repository_options));
    ASSERT_TRUE(repository) << repository.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(
        repository->write("README.md", bytes_of("searched on zetta " + std::string(alpha_term)))));
    const auto change = liquid::runtime::block_on(repository->cut("searched"));
    ASSERT_TRUE(change);
    ASSERT_TRUE(liquid::runtime::block_on(repository->set_ref("main", *change)));

    // The workspace's data key, as lake unwraps it for the index.
    auto document = liquid::runtime::block_on(
        client->get_all(std::string(lake::control::workspace_store_key_object)));
    ASSERT_TRUE(document) << document.error().message;
    auto wrapping = lake::control::WorkspaceStoreCipher::wrapping_key(kek, workspace);
    ASSERT_TRUE(wrapping);
    auto unwrapped = lake::control::WorkspaceStoreCipher::unwrap(*wrapping, workspace, *document);
    ASSERT_TRUE(unwrapped);
    const auto cipher = std::make_shared<const lake::control::WorkspaceStoreCipher>(
        workspace, unwrapped->first, unwrapped->second);
    std::mutex lane;
    const auto index = [&]() {
        lake::search::WorkspaceIndex output;
        output.customer = "space";
        output.workspace = workspace;
        output.generation = owner->fence();
        output.objects = lake::search::make_sealed_index_objects(client, cipher, owner->fence(), {});
        output.repositories = []() -> lake::search::Result<std::vector<std::string>> {
            return std::vector<std::string>{"searched"};
        };
        return output;
    };
    const auto configure = [&](lake::search::Service& service) {
        service.set_repository_access([&](const std::string&, const std::string&, bool)
                                          -> lake::search::Result<lake::search::BorrowedRepository> {
            return lake::search::BorrowedRepository{std::make_shared<std::unique_lock<std::mutex>>(lane),
                                                    &*repository, index()};
        });
        lake::search::WorkspaceAccess access;
        access.lend = [&](const std::string&, const std::string&, bool)
            -> lake::search::Result<lake::search::BorrowedWorkspace> {
            return lake::search::BorrowedWorkspace{std::make_shared<std::unique_lock<std::mutex>>(lane),
                                                   index()};
        };
        access.route = [&](const std::string&, const std::string&)
            -> lake::search::Result<std::optional<std::string>> {
            return std::optional<std::string>{workspace};
        };
        access.switched = [&](const std::string&)
            -> lake::search::Result<std::vector<std::pair<std::string, std::string>>> {
            return std::vector<std::pair<std::string, std::string>>{{"space", workspace}};
        };
        service.set_workspace_access(std::move(access));
    };
    lake::control::InMemoryStore control;
    const lake::search::Query query{std::string(alpha_term), "space", "", "", 20, "", false, workspace};
    const auto search_keys = [&]() {
        std::map<std::string, std::string> output;
        auto listed = liquid::runtime::block_on(client->list(std::string(lake::search::workspace_index_prefix)));
        EXPECT_TRUE(listed);
        for (const auto& object : listed ? *listed : std::vector<lake::control::ObjectListEntry>{}) {
            auto bytes = liquid::runtime::block_on(client->get_all(object.key));
            EXPECT_TRUE(bytes) << object.key;
            output[object.key] = bytes ? *bytes : std::string();
        }
        return output;
    };
    const auto root = zetta.directory / "search-root";
    {
        lake::search::Service service(root);
        configure(service);
        // Without a scheduler the rebuild runs once the search let go of the lane.
        const auto building = service.query(query, "admin", control);
        ASSERT_TRUE(building) << building.error().message;
        EXPECT_EQ(building->building_workspaces, (std::vector<std::string>{workspace}));
        const auto built = service.query(query, "admin", control);
        ASSERT_TRUE(built) << built.error().message;
        ASSERT_EQ(built->items.size(), 1U);
    }
    const auto sealed = search_keys();
    ASSERT_TRUE(sealed.contains("search/manifest.json"));
    EXPECT_TRUE(std::ranges::any_of(sealed, [](const auto& item) { return item.first.starts_with("search/runs/"); }));
    EXPECT_FALSE(holds_text(sealed, alpha_term)) << "the index holds only ciphertext in the Zetta";
    EXPECT_FALSE(holds_text(sealed, "README.md"));
    EXPECT_FALSE(std::filesystem::exists(root));
    {
        lake::search::Service reopened(root);
        configure(reopened);
        const auto reloaded = reopened.query(query, "admin", control);
        ASSERT_TRUE(reloaded) << reloaded.error().message;
        EXPECT_EQ(reloaded->items.size(), 1U);
        EXPECT_EQ(search_keys(), sealed) << "a reload is not a rebuild";
    }
    for (const auto& [key, bytes] : sealed) {
        ASSERT_TRUE(liquid::runtime::block_on(client->remove(key, owner->fence())));
    }
    lake::search::Service rebuilt(root);
    configure(rebuilt);
    ASSERT_TRUE(rebuilt.query(query, "admin", control));
    const auto answered = rebuilt.query(query, "admin", control);
    ASSERT_TRUE(answered) << answered.error().message;
    EXPECT_EQ(answered->items.size(), 1U) << "rebuilt, not served empty";
    EXPECT_TRUE(search_keys().contains("search/manifest.json"));
    repository = std::unexpected(liquid::error{liquid::errc::not_found, "closed", ""});
    ASSERT_TRUE(liquid::runtime::block_on(store->close()));
}

// ---- lake#527: what one commit costs on a workspace store -------------------------------------

// Passes every exchange through and keeps "METHOD path?query" of each, for the request trace.
class TracingTransport final : public lake::control::ObjectTransport {
public:
    explicit TracingTransport(std::shared_ptr<lake::control::ObjectTransport> inner) : inner_(std::move(inner)) {}

    liquid::runtime::task<lake::control::ObjectResponse> exchange(lake::control::ObjectRequest request) override {
        std::string line = request.method + " ";
        const auto objects = request.url.find("/v1/");
        line += objects == std::string::npos ? request.url : request.url.substr(objects);
        {
            std::lock_guard lock(mutex_);
            // A request that asks Zetta to fsync before it answers.
            if (line.find(":barrier") != std::string::npos || line.find("durable=true") != std::string::npos) {
                ++durable_;
            }
            if (recording_) {
                trace_.push_back(std::move(line));
            }
        }
        {
            std::lock_guard lock(mutex_);
            // A request sent while none is in flight starts a new round trip.
            if (in_flight_++ == 0) {
                ++rounds_;
            }
        }
        auto answered = co_await inner_->exchange(std::move(request));
        {
            std::lock_guard lock(mutex_);
            --in_flight_;
        }
        co_return answered;
    }
    std::uint64_t rounds() const {
        std::lock_guard lock(mutex_);
        return rounds_;
    }
    std::uint64_t durable() const {
        std::lock_guard lock(mutex_);
        return durable_;
    }

    void record(bool on) {
        std::lock_guard lock(mutex_);
        recording_ = on;
        if (on) {
            trace_.clear();
        }
    }
    std::vector<std::string> trace() const {
        std::lock_guard lock(mutex_);
        return trace_;
    }

private:
    std::shared_ptr<lake::control::ObjectTransport> inner_;
    mutable std::mutex mutex_;
    bool recording_ = false;
    std::vector<std::string> trace_;
    int in_flight_ = 0;
    std::uint64_t rounds_ = 0;
    std::uint64_t durable_ = 0;
};

std::chrono::milliseconds measured_round_trip() {
    const char* value = std::getenv("LAKE_TEST_ZETTA_RTT_MS");
    return std::chrono::milliseconds(value != nullptr && *value != '\0' ? std::atoi(value) : 10);
}

// What an fsync costs the workspace Zetta's disk in the measurement (lake#535): the proxy holds
// each durable request this much longer. Dev's Azure disk answered about 30 ms per durable
// request (lake#535); 0, the default, leaves it to the loopback disk.
std::chrono::milliseconds measured_fsync_cost() {
    const char* value = std::getenv("LAKE_TEST_ZETTA_FSYNC_MS");
    return std::chrono::milliseconds(value != nullptr && *value != '\0' ? std::atoi(value) : 0);
}

// What lake's commit_repository_files does for one regular file (SyncStore::commit_repository_files):
// remove the path (absent is fine), write it, cut the change.
liquid::result<liquid::change_id> commit_one_file(liquid::repository& repository, const std::string& path,
                                                  std::vector<std::byte> contents) {
    auto removed = liquid::runtime::block_on(repository.remove(path));
    if (!removed && removed.error().code != liquid::errc::not_found) {
        return std::unexpected(removed.error());
    }
    auto written = liquid::runtime::block_on(repository.write(path, std::move(contents)));
    if (!written) {
        return std::unexpected(written.error());
    }
    return liquid::runtime::block_on(repository.cut("one file"));
}

// One-file commits into a repository of a workspace store on a real Zetta behind a proxy that adds
// LAKE_TEST_ZETTA_RTT_MS (default 10) of round trip, as the cluster's network does: the requests
// of each kind one commit sends, and its time. The fourth commit is the steady state (the store
// and the repository open, the directory and headers known).
TEST(WorkspaceStoreZetta, AOneFileCommitsRequestsAreCountedAndTimed) {
    RealZetta zetta;
    if (!zetta.start()) {
        GTEST_SKIP() << "set LAKE_TEST_ZETTA_BINARY to a zetta built from origin/dev";
    }
    const auto round_trip = measured_round_trip();
    const auto fsync_cost = measured_fsync_cost();
    lake::test::LatencyProxy proxy(zetta.port, round_trip, fsync_cost);
    auto tracing = std::make_shared<TracingTransport>(lake::control::make_zetta_object_transport());
    const std::string workspace = lake::control::new_uuid_v4();
    auto client = std::make_shared<ZettaObjectClient>(
        tracing, ObjectEndpoint{"http://127.0.0.1:" + std::to_string(proxy.port()), zetta.token});
    ASSERT_TRUE(liquid::runtime::block_on(client->create_bucket(0)));
    auto owner = std::make_shared<ZettaStoreOwner>(client, 30s);
    liquid::store_options options;
    auto backend = std::make_shared<ZettaObjectBackend>(
        client, ZettaObjectBackendOptions{.workspace_id = workspace,
                                          .instance_kek = std::string(kek),
                                          .create_key = true});
    options.backend = backend;
    options.owner = owner;
    auto store = liquid::runtime::block_on(liquid::store::open(zetta.directory / "unused", std::move(options)));
    ASSERT_TRUE(store) << store.error().message;
    const auto identifier = zetta.directory / "repositories" / "measured";
    liquid::repository_options repository_options;
    repository_options.store = &*store;
    auto repository = liquid::runtime::block_on(liquid::repository::create(identifier, repository_options));
    ASSERT_TRUE(repository) << repository.error().message;

    // A repository with history and directories first, as on dev (lake#535):
    // LAKE_TEST_ZETTA_SEED_COMMITS commits of 8 files each, spread over 16 directories.
    if (const char* seeded = std::getenv("LAKE_TEST_ZETTA_SEED_COMMITS"); seeded != nullptr) {
        for (int round = 0; round < std::atoi(seeded); ++round) {
            for (int file = 0; file < 8; ++file) {
                const std::string path = "seed/dir-" + std::to_string((round * 8 + file) % 16) + "/file-" +
                                         std::to_string(round) + "-" + std::to_string(file) + ".md";
                ASSERT_TRUE(liquid::runtime::block_on(repository->write(path, random_bytes(2000, round * 8 + file))));
            }
            ASSERT_TRUE(liquid::runtime::block_on(repository->cut("seed " + std::to_string(round))));
        }
        ASSERT_TRUE(liquid::runtime::block_on(backend->make_durable()));
    }

    std::printf("\nlake#527, lake#535: one-file commits on a workspace store, real Zetta, %lld ms round "
                "trip, %lld ms per durable request\n",
                static_cast<long long>(round_trip.count()), static_cast<long long>(fsync_cost.count()));
    std::printf("| Commit | ms | in requests, ms | requests | durable | sequential rounds | by kind |\n"
                "|---|---:|---:|---:|---:|---:|---|\n");
    double steady_ms = 0;
    std::uint64_t steady_requests = 0;
    std::uint64_t steady_durable = 0;
    for (int commit = 1; commit <= 4; ++commit) {
        lake::control::ObjectRequestTally tally;
        std::array<std::uint64_t, lake::control::object_request_kinds> count{};
        std::array<std::uint64_t, lake::control::object_request_kinds> nanoseconds{};
        for (std::size_t index = 0; index < count.size(); ++index) {
            count[index] = client->counters()->by_kind.count[index].load();
            nanoseconds[index] = client->counters()->by_kind.nanoseconds[index].load();
        }
        const std::uint64_t rounds_before = tracing->rounds();
        tracing->record(commit == 4);
        const auto started = std::chrono::steady_clock::now();
        const std::uint64_t durable_before = tracing->durable();
        auto change = commit_one_file(*repository, "notes/file-" + std::to_string(commit) + ".md",
                                      bytes_of("a small note, commit " + std::to_string(commit) + "\n"));
        ASSERT_TRUE(change) << change.error().message;
        // Lake's sync before it answers (lake#535): the commit is acknowledged once it returns.
        ASSERT_TRUE(liquid::runtime::block_on(backend->make_durable()));
        const double elapsed = milliseconds_since(started);
        tally.add_difference(client->counters()->by_kind, count, nanoseconds);
        tracing->record(false);
        const std::uint64_t rounds = tracing->rounds() - rounds_before;
        const std::uint64_t durable = tracing->durable() - durable_before;
        std::printf("| %d | %.0f | %.0f | %llu | %llu | %llu | %s |\n", commit, elapsed,
                    static_cast<double>(tally.total_nanoseconds()) / 1e6,
                    static_cast<unsigned long long>(tally.total_requests()),
                    static_cast<unsigned long long>(durable), static_cast<unsigned long long>(rounds),
                    tally.summary().c_str());
        if (commit == 4) {
            steady_ms = elapsed;
            steady_requests = tally.total_requests();
            steady_durable = durable;
        }
    }
    std::printf("\nrequests of the steady-state commit, in order:\n");
    for (const std::string& line : tracing->trace()) {
        std::printf("  %s\n", line.c_str());
    }
    std::printf("steady state: %.0f ms, %llu requests, %llu durable\n", steady_ms,
                static_cast<unsigned long long>(steady_requests),
                static_cast<unsigned long long>(steady_durable));
    EXPECT_EQ(steady_durable, 1U) << "one :barrier per commit (lake#535)";
    if (fsync_cost.count() > 0) {
        EXPECT_EQ(proxy.durable_requests(), tracing->durable()) << "the proxy saw every durable request";
    }
    ASSERT_TRUE(liquid::runtime::block_on(store->close()));
}

// ---- lake#527: a crash keeps what the last barrier acknowledged --------------------------------

using ObjectState = std::optional<std::vector<std::byte>>;

bool is_prefix(const std::vector<std::byte>& shorter, const std::vector<std::byte>& longer) {
    return shorter.size() <= longer.size() && std::equal(shorter.begin(), shorter.end(), longer.begin());
}

// liquid's operations in the order it issued them: each with its key and the key's value after it,
// and whether it was an append.
struct IssuedOperation {
    std::string key;
    ObjectState after;
    bool append = false;
};

// A crash keeps a prefix of the issue order (lake#527): some p at or after `acknowledged` (the
// operations the last answered barrier covered) such that every key holds its value after the
// first p operations, except that a key whose operations p+1..q are consecutive may hold any
// value between its value after one of them and its value after q, when only appends lie between
// (an append is sent in whole blocks).
bool a_prefix_survived(const std::vector<std::string>& keys,
                       const std::vector<IssuedOperation>& issued,
                       std::size_t acknowledged,
                       const std::map<std::string, ObjectState>& found) {
    const auto value_after = [&](const std::string& key, std::size_t count) {
        ObjectState value;
        for (std::size_t index = 0; index < count; ++index) {
            if (issued[index].key == key) {
                value = issued[index].after;
            }
        }
        return value;
    };
    for (std::size_t p = acknowledged; p <= issued.size(); ++p) {
        std::size_t q = p;
        while (q < issued.size() && issued[q].key == (p < issued.size() ? issued[p].key : std::string())) {
            ++q;
        }
        bool matches = true;
        for (const std::string& key : keys) {
            const ObjectState& have = found.at(key);
            if (have == value_after(key, p)) {
                continue;
            }
            bool between = false;
            if (p < issued.size() && issued[p].key == key && have) {
                for (std::size_t r = p; r <= q && !between; ++r) {
                    const ObjectState start = value_after(key, r);
                    // Through the appends that follow the r-th operation, and no further.
                    for (std::size_t end = r + 1; end <= q && issued[end - 1].append && !between; ++end) {
                        const ObjectState stop = value_after(key, end);
                        between = start && stop && is_prefix(*start, *have) && is_prefix(*have, *stop);
                    }
                }
            }
            if (!between) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return true;
        }
    }
    return false;
}

// Random puts (held and sent at once), appends past the flush threshold, conditional creates and
// removes, with liquid's barriers and lake's syncs between (lake#535); then a sync that is
// answered, stopped part way, or loses requests or answers at random; then a crash that keeps a
// prefix of the bucket's log. What survives is a prefix of liquid's issue order no shorter than
// the last answered sync.
TEST(WorkspaceStoreBackend, ACrashAnywhereKeepsAPrefixOfTheIssueOrderPastTheLastBarrier) {
    const std::vector<std::string> keys{"packs/pack-1.pack", "index/run-1.idx", "index/MANIFEST",
                                        "packs/objects.meta", "repositories/r/LIQUID_REPO",
                                        "leases/a.lease"};
    for (std::uint64_t seed = 1; seed <= 200; ++seed) {
        Rig rig;
        ZettaObjectBackendOptions options = small_blocks();
        options.deferred_put_bytes = 2048; // larger puts take the immediate path
        options.held_puts_limit = 3;       // and the bound is crossed now and then
        auto backend = rig.backend(options);
        std::mt19937_64 random(seed);
        std::vector<IssuedOperation> issued;
        std::size_t acknowledged = 0;
        std::map<std::string, ObjectState> current;
        for (const std::string& key : keys) {
            current[key] = std::nullopt;
        }
        const int steps = 10 + static_cast<int>(random() % 40);
        for (int step = 0; step < steps; ++step) {
            const std::string& key = keys[random() % keys.size()];
            const auto choice = random() % 10;
            if (choice < 2) {
                if (random() % 2 == 0) {
                    // Liquid's barrier: a point in the issue order that acknowledges nothing.
                    ASSERT_TRUE(liquid::runtime::block_on(backend->barrier())) << seed;
                    continue;
                }
                // Lake's sync at the end of an operation: everything before it is durable.
                ASSERT_TRUE(liquid::runtime::block_on(backend->sync())) << seed;
                acknowledged = issued.size();
                continue;
            }
            bool append = false;
            if (choice < 5) {
                // Sometimes the same bytes again, as liquid republishes its index manifest.
                auto bytes = current[key] && random() % 3 == 0 ? *current[key]
                                                               : random_bytes(random() % 5000, random());
                ASSERT_TRUE(liquid::runtime::block_on(backend->put(key, bytes))) << seed;
                current[key] = std::move(bytes);
            } else if (choice < 8 && current[key]) {
                const auto more = random_bytes(1 + random() % 3000, random());
                ASSERT_TRUE(liquid::runtime::block_on(backend->put_range(key, current[key]->size(), more)))
                    << seed;
                current[key]->insert(current[key]->end(), more.begin(), more.end());
                append = true;
            } else if (current[key]) {
                ASSERT_TRUE(liquid::runtime::block_on(backend->remove(key))) << seed;
                current[key] = std::nullopt;
            } else {
                auto bytes = random_bytes(random() % 1500, random());
                ASSERT_TRUE(liquid::runtime::block_on(backend->put_if_absent(key, bytes))) << seed;
                current[key] = std::move(bytes);
            }
            issued.push_back({key, current[key], append});
        }
        const auto ending = random() % 4;
        if (ending == 1) {
            rig.zetta->fail_after(static_cast<int>(random() % 8));
        } else if (ending == 2) {
            rig.zetta->fail_randomly(seed, 50);
        } else if (ending == 3) {
            rig.zetta->lose_answers_randomly(seed, 50);
        }
        if (ending != 0 || random() % 2 == 0) {
            if (liquid::runtime::block_on(backend->sync())) {
                acknowledged = issued.size();
            }
        }
        rig.zetta->fail_after(-1);
        rig.zetta->fail_randomly(0, 0);
        rig.zetta->lose_answers_randomly(0, 0);
        rig.zetta->crash(rig.endpoint, seed);
        auto reader = rig.backend(small_blocks(), false);
        std::map<std::string, ObjectState> found;
        for (const std::string& key : keys) {
            found[key] = std::nullopt;
            auto listed = liquid::runtime::block_on(reader->list(key));
            ASSERT_TRUE(listed) << seed << " " << listed.error().message;
            for (const auto& entry : *listed) {
                if (entry.key == key) {
                    auto bytes = liquid::runtime::block_on(reader->get_range(key, 0, entry.size));
                    ASSERT_TRUE(bytes) << seed << " " << key << " " << bytes.error().message;
                    found[key] = std::move(*bytes);
                }
            }
        }
        std::string sizes;
        for (const std::string& key : keys) {
            sizes += " " + key + "=" + (found[key] ? std::to_string(found[key]->size()) : std::string("absent"));
        }
        EXPECT_TRUE(a_prefix_survived(keys, issued, acknowledged, found))
            << "seed " << seed << ": no prefix of the " << issued.size() << " issued operations (at least "
            << acknowledged << ") gives" << sizes;
        if (std::getenv("LAKE_TEST_TRACE_ISSUED") != nullptr && !a_prefix_survived(keys, issued, acknowledged, found)) {
            for (const auto& operation : issued) {
                std::printf("  %s%s -> %s\n", operation.append ? "append " : "", operation.key.c_str(),
                            operation.after ? std::to_string(operation.after->size()).c_str() : "absent");
            }
            for (const auto& line : rig.zetta->applied()) {
                std::printf("  applied %s\n", line.c_str());
            }
        }
    }
}

// The writes of an operation are sent one after another, in the order liquid issued them, never
// two at once, so the bucket can never hold a later one without an earlier one. Liquid's barriers
// between them send nothing (lake#535); none is durable until the sync.
TEST(WorkspaceStoreBackend, AnIntervalsWritesAreSentOneAtATimeInIssueOrder) {
    Rig rig;
    auto backend = rig.backend(small_blocks());
    ASSERT_TRUE(liquid::runtime::block_on(backend->put("objects/old", bytes_of("old"))));
    ASSERT_TRUE(liquid::runtime::block_on(backend->sync()));
    const std::size_t before = rig.zetta->applied().size();
    const std::uint64_t durable_before = rig.zetta->durable_requests(rig.endpoint);
    rig.zetta->set_latency(3ms);
    ASSERT_TRUE(liquid::runtime::block_on(backend->put("gc/marker", bytes_of("pending"))));
    ASSERT_TRUE(liquid::runtime::block_on(backend->barrier()));
    ASSERT_TRUE(liquid::runtime::block_on(backend->put("packs/pack-9.pack", random_bytes(1500, 1))));
    ASSERT_TRUE(liquid::runtime::block_on(backend->barrier()));
    ASSERT_TRUE(liquid::runtime::block_on(backend->put("index/run-9.idx", random_bytes(40, 2))));
    ASSERT_TRUE(liquid::runtime::block_on(backend->put_range("index/run-9.idx", 40, random_bytes(900, 3))));
    ASSERT_TRUE(liquid::runtime::block_on(backend->barrier()));
    EXPECT_EQ(rig.zetta->applied().size(), before) << "liquid's barriers send nothing";
    ASSERT_TRUE(liquid::runtime::block_on(backend->remove("objects/old")));
    ASSERT_TRUE(liquid::runtime::block_on(backend->barrier()));
    ASSERT_TRUE(liquid::runtime::block_on(backend->put("index/MANIFEST", bytes_of("manifest"))));
    ASSERT_TRUE(liquid::runtime::block_on(backend->barrier()));
    EXPECT_EQ(rig.zetta->durable_requests(rig.endpoint), durable_before);
    ASSERT_TRUE(liquid::runtime::block_on(backend->sync()));
    rig.zetta->set_latency(0ms);
    const auto applied = rig.zetta->applied();
    std::vector<std::string> keys;
    for (std::size_t index = before; index < applied.size(); ++index) {
        keys.push_back(applied[index].substr(applied[index].find(' ') + 1));
    }
    EXPECT_EQ(keys, (std::vector<std::string>{"gc/marker", "packs/pack-9.pack", "index/run-9.idx",
                                             "objects/old", "index/MANIFEST"}));
    EXPECT_EQ(rig.zetta->durable_requests(rig.endpoint), durable_before + 1);
    EXPECT_EQ(rig.zetta->most_writes_in_flight(), 1);
}

// A write whose answer was lost after Zetta applied it: a durable PUT and a DELETE are retried
// and change nothing more; a conditional create that landed answers unavailable, fails the
// backend, and the store reopened finds the object.
TEST(WorkspaceStoreBackend, AWriteWhoseAnswerWasLostIsRetriedOrFailsTheBackend) {
    Rig rig;
    auto backend = rig.backend(small_blocks());
    ASSERT_TRUE(liquid::runtime::block_on(backend->put("objects/a", bytes_of("first"))));
    ASSERT_TRUE(liquid::runtime::block_on(backend->put("objects/b", bytes_of("second"))));
    ASSERT_TRUE(liquid::runtime::block_on(backend->sync()));
    // The held put's durable PUT lands, its answer is lost, the retry lands again.
    ASSERT_TRUE(liquid::runtime::block_on(backend->put("objects/a", bytes_of("replaced"))));
    rig.zetta->lose_next_answers(1);
    ASSERT_TRUE(liquid::runtime::block_on(backend->sync()));
    EXPECT_EQ(read(*backend, "objects/a", 0, 8), bytes_of("replaced"));
    // The DELETE lands, its answer is lost, the retry answers 404, which is the removal.
    rig.zetta->lose_next_answers(1);
    ASSERT_TRUE(liquid::runtime::block_on(backend->remove("objects/b")));
    EXPECT_FALSE(rig.zetta->objects(rig.endpoint).contains("objects/b"));
    EXPECT_FALSE(backend->failed());
    rig.zetta->crash(rig.endpoint, 1);
    auto reopened = rig.backend(small_blocks(), false);
    EXPECT_EQ(read(*reopened, "objects/a", 0, 8), bytes_of("replaced"));
    EXPECT_EQ(size_of(*reopened, "objects/b"), 0U);

    // A conditional create that landed but whose answer was lost is not retried: unavailable,
    // the backend failed (lake reopens the store), and the reopened store holds the object.
    rig.zetta->lose_next_answers(1);
    const auto created = liquid::runtime::block_on(backend->put_if_absent("objects/c", bytes_of("made")));
    ASSERT_FALSE(created);
    EXPECT_EQ(created.error().code, liquid::errc::unavailable);
    EXPECT_TRUE(backend->failed());
    auto after = rig.backend(small_blocks(), false);
    EXPECT_EQ(read(*after, "objects/c", 0, 4), bytes_of("made"));
    const auto again = liquid::runtime::block_on(after->put_if_absent("objects/c", bytes_of("made")));
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error().code, liquid::errc::already_exists);
}

// Held writes are bounded: past the count or the bytes they are sent early, without durability,
// and the next barrier sends :barrier.
TEST(WorkspaceStoreBackend, HeldWritesPastTheirBoundAreSentEarly) {
    Rig rig;
    ZettaObjectBackendOptions options = small_blocks();
    options.held_puts_limit = 4;
    options.held_bytes_limit = 6000;
    auto backend = rig.backend(options);
    ASSERT_TRUE(liquid::runtime::block_on(backend->sync()));
    for (int index = 0; index < 5; ++index) {
        ASSERT_TRUE(liquid::runtime::block_on(backend->put("objects/" + std::to_string(index), bytes_of("x"))));
    }
    EXPECT_TRUE(rig.zetta->objects(rig.endpoint).contains("objects/0")) << "the fifth put sent the four held";
    EXPECT_FALSE(rig.zetta->objects(rig.endpoint).contains("objects/4"));
    const auto barriers = rig.zetta->barriers(rig.endpoint);
    ASSERT_TRUE(liquid::runtime::block_on(backend->sync()));
    EXPECT_EQ(rig.zetta->barriers(rig.endpoint), barriers + 1) << "an early send needs :barrier";
    ASSERT_TRUE(liquid::runtime::block_on(backend->put("objects/big-1", random_bytes(2000, 4))));
    ASSERT_TRUE(liquid::runtime::block_on(backend->put("objects/big-2", random_bytes(2000, 5))));
    ASSERT_TRUE(liquid::runtime::block_on(backend->put("objects/big-3", random_bytes(2500, 6))));
    ASSERT_TRUE(liquid::runtime::block_on(backend->put("objects/big-4", random_bytes(10, 7))));
    EXPECT_TRUE(rig.zetta->objects(rig.endpoint).contains("objects/big-3")) << "past 6,000 held bytes";
    ASSERT_TRUE(liquid::runtime::block_on(backend->sync()));
    EXPECT_EQ(rig.zetta->barriers(rig.endpoint), barriers + 2);
}

// The same through liquid: one-file commits as lake makes them (each acknowledged once lake's sync
// after it answers, lake#535), then one the workspace Zetta stops answering part way through, then
// a crash. The store reopens (a successor with a higher fence) and every commit that was
// acknowledged reads back.
TEST(WorkspaceStoreBackend, ACrashMidCommitKeepsEveryAcknowledgedCommit) {
    for (std::uint64_t seed = 1; seed <= 48; ++seed) {
        // Checkpoints every few drips (a one-file commit makes more than one) and liquid's default
        // (none here): a checkpoint publishes LIQUID_REPO and then removes LIQUID_JOURNAL with no
        // barrier after, and a later open prefers the journal while it exists.
        const std::uint64_t interval = std::array<std::uint64_t, 6>{1, 2, 3, 4, 5, 64}[seed % 6];
        Rig rig;
        const auto root = lake::test::fixture_path("workspace-store-crash-" + rig.workspace);
        const auto identifier = root / "repositories" / "crashed";
        std::mt19937_64 random(seed);
        std::vector<std::pair<std::string, std::vector<std::byte>>> acknowledged;
        std::optional<liquid::change_id> head;
        // The interrupted commit when liquid answered it but the sync did not: not acknowledged,
        // so the crash may keep it or not.
        std::optional<liquid::change_id> unacknowledged;
        {
            std::shared_ptr<ZettaStoreOwner> owner;
            auto store_options = owned(rig, owner);
            auto backend = std::static_pointer_cast<ZettaObjectBackend>(store_options.backend);
            auto store = liquid::runtime::block_on(liquid::store::open(root, std::move(store_options)));
            ASSERT_TRUE(store) << store.error().message;
            liquid::repository_options options;
            options.store = &*store;
            options.checkpoint_interval = interval;
            auto repository = liquid::runtime::block_on(liquid::repository::create(identifier, options));
            ASSERT_TRUE(repository) << repository.error().message;
            const int commits = 2 + static_cast<int>((seed / 6) % 4);
            for (int commit = 0; commit < commits; ++commit) {
                const std::string path = "notes/" + std::to_string(commit) + ".md";
                auto bytes = random_bytes(10 + random() % 3000, random());
                auto change = commit_one_file(*repository, path, bytes);
                ASSERT_TRUE(change) << seed;
                ASSERT_TRUE(liquid::runtime::block_on(backend->make_durable())) << seed;
                head = *change;
                acknowledged.emplace_back(path, std::move(bytes));
            }
            // The process stops part way, loses requests, or loses answers to writes that landed.
            if (seed % 4 < 2) {
                // Often at once: the commit acknowledged last must hold without anything after it.
                rig.zetta->fail_after(static_cast<int>(seed % 8 == 0 ? 0 : random() % 60));
            } else if (seed % 4 == 2) {
                rig.zetta->fail_randomly(seed, 40);
            } else {
                rig.zetta->lose_answers_randomly(seed, 40);
            }
            auto bytes = random_bytes(10 + random() % 3000, random());
            if (auto change = commit_one_file(*repository, "notes/interrupted.md", bytes)) {
                if (liquid::runtime::block_on(backend->make_durable())) {
                    head = *change;
                    acknowledged.emplace_back("notes/interrupted.md", std::move(bytes));
                } else {
                    unacknowledged = *change;
                }
            }
            // The process stops: nothing more reaches the workspace Zetta.
            rig.zetta->fail_randomly(0, 0);
            rig.zetta->lose_answers_randomly(0, 0);
            rig.zetta->fail_after(0);
            owner->stop_renewing();
        }
        rig.zetta->fail_after(-1);
        rig.zetta->crash(rig.endpoint, seed);
        rig.zetta->expire(rig.endpoint);
        std::shared_ptr<ZettaStoreOwner> successor;
        auto store = liquid::runtime::block_on(liquid::store::open(root, owned(rig, successor)));
        ASSERT_TRUE(store) << "seed " << seed << ": " << store.error().message;
        liquid::repository_options options;
        options.store = &*store;
        options.checkpoint_interval = interval;
        auto repository = liquid::runtime::block_on(liquid::repository::open(identifier, options));
        ASSERT_TRUE(repository) << "seed " << seed << ": " << repository.error().message;
        // The last acknowledged change is the head: a stale LIQUID_JOURNAL (the state between a
        // commit's write and its cut) would hold its file but not its change.
        auto reopened_head = liquid::runtime::block_on(repository->head());
        ASSERT_TRUE(reopened_head) << "seed " << seed << ": " << reopened_head.error().message;
        if (!unacknowledged ||
            liquid::to_string(reopened_head->value) != liquid::to_string(unacknowledged->value)) {
            EXPECT_EQ(liquid::to_string(reopened_head->value), liquid::to_string(head->value)) << "seed " << seed;
        }
        for (const auto& [path, bytes] : acknowledged) {
            auto read_back = liquid::runtime::block_on(repository->read(path));
            ASSERT_TRUE(read_back) << "seed " << seed << ": " << path << ": " << read_back.error().message;
            EXPECT_EQ(*read_back, bytes) << "seed " << seed << ": " << path;
        }
        ASSERT_TRUE(liquid::runtime::block_on(store->close()));
    }
}

// A barrier's concurrent requests: no job starts after a failure, and a lost lease is the answer
// whatever else failed.
TEST(WorkspaceStoreBackend, ConcurrentRequestsStopAtTheFirstFailureAndReportALostLeaseFirst) {
    using Job = std::function<liquid::runtime::task<void>()>;
    const auto failing = [](liquid::errc code) -> Job {
        return [code]() -> liquid::runtime::task<void> {
            co_return std::unexpected(liquid::error{code, "test", {}});
        };
    };
    std::atomic<int> ran{0};
    const Job counted = [&ran]() -> liquid::runtime::task<void> {
        ran.fetch_add(1);
        co_return liquid::result<void>{};
    };
    std::vector<Job> serial{failing(liquid::errc::unavailable), counted, counted};
    const auto first = liquid::runtime::block_on(lake::control::run_object_requests(serial, 1));
    ASSERT_FALSE(first);
    EXPECT_EQ(first.error().code, liquid::errc::unavailable);
    EXPECT_EQ(ran.load(), 0) << "nothing starts after a failure";
    // Both in flight: the unavailable one fails first, the locked one after.
    const auto later = [](liquid::errc code, std::chrono::milliseconds after) -> Job {
        return [code, after]() -> liquid::runtime::task<void> {
            auto waited = co_await liquid::runtime::offload([after]() -> int {
                std::this_thread::sleep_for(after);
                return 0;
            });
            static_cast<void>(waited);
            co_return std::unexpected(liquid::error{code, "test", {}});
        };
    };
    std::vector<Job> both{later(liquid::errc::unavailable, 5ms), later(liquid::errc::locked, 40ms)};
    const auto locked = liquid::runtime::block_on(lake::control::run_object_requests(both, 2));
    ASSERT_FALSE(locked);
    EXPECT_EQ(locked.error().code, liquid::errc::locked);
}

// Release settles what the backend holds within a bound: with the workspace Zetta not answering
// those writes, the drain does not stall, and the lease is still released.
TEST(WorkspaceStoreOwner, ReleaseSettlesWithinItsBudgetWhenZettaDoesNotAnswer) {
    Rig rig;
    auto client = rig.client();
    ASSERT_TRUE(liquid::runtime::block_on(client->create_bucket(0)));
    auto owner = std::make_shared<ZettaStoreOwner>(client, 30s, std::function<void()>{}, 300ms);
    liquid::store_options options;
    options.backend = std::make_shared<ZettaObjectBackend>(
        client, ZettaObjectBackendOptions{.workspace_id = rig.workspace,
                                          .instance_kek = std::string(kek),
                                          .create_key = true});
    options.owner = owner;
    const auto root = lake::test::fixture_path("workspace-store-settle-" + rig.workspace);
    auto store = liquid::runtime::block_on(liquid::store::open(root, options));
    ASSERT_TRUE(store) << store.error().message;
    auto view = options.backend->fenced(owner->fence());
    ASSERT_TRUE(view);
    ASSERT_TRUE(liquid::runtime::block_on((*view)->put("objects/held", bytes_of("held"))));
    rig.zetta->hang(rig.endpoint, 20s, "/objects/held");
    const auto started = std::chrono::steady_clock::now();
    const auto released = liquid::runtime::block_on(owner->release());
    const auto took = std::chrono::steady_clock::now() - started;
    EXPECT_LT(took, 3s);
    EXPECT_FALSE(released) << "what was held could not be sent";
    EXPECT_TRUE(rig.zetta->lease(rig.endpoint).holder.empty()) << "the lease is released anyway";
    rig.zetta->hang(rig.endpoint, 0ms);
}

// The settling's budget cuts only its own requests: another user of the client (search writes
// its index with the store's client) keeps its timeout and retries meanwhile.
TEST(WorkspaceStoreOwner, TheSettleBudgetCutsOnlyTheSettlingRequests) {
    Rig rig;
    auto client = rig.client();
    ASSERT_TRUE(liquid::runtime::block_on(client->create_bucket(0)));
    client->set_before_release([client]() -> liquid::runtime::task<void> {
        // The settling's own request: cut to the budget.
        co_return co_await client->put("objects/settled", "bytes", std::nullopt, false);
    });
    rig.zetta->hang(rig.endpoint, 1500ms, "/objects/");
    liquid::result<void> other;
    std::thread beside([&] {
        other = liquid::runtime::block_on(client->put("search/manifest.json", "sealed", std::nullopt, false));
    });
    std::this_thread::sleep_for(50ms);
    const auto settled = liquid::runtime::block_on(client->before_release(500ms));
    beside.join();
    rig.zetta->hang(rig.endpoint, 0ms);
    EXPECT_FALSE(settled) << "the settling request ran past its budget";
    EXPECT_TRUE(other) << "a request beside the settling is not cut: " << (other ? "" : other.error().message);
}

// What is held is counted exactly: a put replacing a held put, a removal of a held key, and a
// threshold flush leave only what is still held.
TEST(WorkspaceStoreBackend, HeldBytesAndPutsStayExact) {
    Rig rig;
    auto backend = rig.backend(small_blocks());
    ASSERT_TRUE(liquid::runtime::block_on(backend->sync()));
    ASSERT_TRUE(liquid::runtime::block_on(backend->put("objects/a", random_bytes(300, 1))));
    ASSERT_TRUE(liquid::runtime::block_on(backend->put("objects/a", random_bytes(200, 2))));
    EXPECT_EQ(backend->held(), (std::pair<std::uint64_t, std::size_t>{200, 1})) << "the second put replaced the first";
    ASSERT_TRUE(liquid::runtime::block_on(backend->put("objects/b", random_bytes(100, 3))));
    ASSERT_TRUE(liquid::runtime::block_on(backend->remove("objects/b")));
    EXPECT_EQ(backend->held(), (std::pair<std::uint64_t, std::size_t>{200, 1})) << "a held key removed is not held";
    ASSERT_TRUE(liquid::runtime::block_on(backend->sync()));
    EXPECT_EQ(backend->held(), (std::pair<std::uint64_t, std::size_t>{0, 0}));
    // An object in Zetta appended past the flush threshold (4 KiB here) keeps only its partial block.
    ASSERT_TRUE(liquid::runtime::block_on(backend->put_range("packs/pack-1.pack", 0, {})));
    ASSERT_TRUE(liquid::runtime::block_on(backend->sync()));
    ASSERT_TRUE(liquid::runtime::block_on(backend->put_range("packs/pack-1.pack", 0, random_bytes(5000, 4))));
    EXPECT_EQ(backend->held(), (std::pair<std::uint64_t, std::size_t>{5000 - 4096, 0}));
    ASSERT_TRUE(liquid::runtime::block_on(backend->sync()));
    EXPECT_EQ(backend->held(), (std::pair<std::uint64_t, std::size_t>{0, 0}));
}

// A stale tail whose name the barrier writes again (one left for a later block) is never removed
// once the barrier has written it: stale tails go after the barrier's writes, and none an object
// holds.
TEST(WorkspaceStoreBackend, ABarrierNeverRemovesATailItJustWrote) {
    Rig rig;
    auto backend = rig.backend(small_blocks());
    ASSERT_TRUE(liquid::runtime::block_on(backend->put_range("packs/pack-1.pack", 0, {})));
    ASSERT_TRUE(liquid::runtime::block_on(backend->sync()));
    std::vector<std::byte> grown = random_bytes(1500, 5); // one block and a tail of block 1
    ASSERT_TRUE(liquid::runtime::block_on(backend->put_range("packs/pack-1.pack", 0, grown)));
    ASSERT_TRUE(liquid::runtime::block_on(backend->sync()));
    const std::string current = tail_of(rig.zetta->objects(rig.endpoint), "packs/pack-1.pack");
    ASSERT_FALSE(current.empty());
    // A tail left for block 2, as an interrupted barrier or a rollback could leave it.
    const std::string later = current.substr(0, current.find(".lake-tail-")) + ".lake-tail-2" +
                              current.substr(current.rfind('-'));
    rig.zetta->set_object(rig.endpoint, later, "left behind");
    auto reopened = rig.backend(small_blocks());
    EXPECT_EQ(size_of(*reopened, "packs/pack-1.pack"), 1500U); // the block-2 tail is stale
    const auto more = random_bytes(1000, 6); // completes block 1, a tail of block 2
    ASSERT_TRUE(liquid::runtime::block_on(reopened->put_range("packs/pack-1.pack", 1500, more)));
    grown.insert(grown.end(), more.begin(), more.end());
    ASSERT_TRUE(liquid::runtime::block_on(reopened->sync()));
    ASSERT_TRUE(liquid::runtime::block_on(reopened->sync()));
    auto reader = rig.backend(small_blocks());
    EXPECT_EQ(size_of(*reader, "packs/pack-1.pack"), grown.size());
    EXPECT_EQ(read(*reader, "packs/pack-1.pack", 0, grown.size()), grown);
}

// The search index's objects in the bucket (lake#381) never enter liquid's directory, and liquid
// cannot write under their prefix.
TEST(WorkspaceStoreBackend, TheSearchIndexsObjectsAreNotLiquids) {
    Rig rig;
    auto backend = rig.backend(small_blocks());
    ASSERT_TRUE(liquid::runtime::block_on(backend->put("objects/a", bytes_of("a"))));
    ASSERT_TRUE(liquid::runtime::block_on(backend->sync()));
    rig.zetta->set_object(rig.endpoint, std::string(lake::control::workspace_store_search_prefix) + "manifest.json",
                          "sealed index");
    auto reopened = rig.backend(small_blocks());
    const auto listed = liquid::runtime::block_on(reopened->list(""));
    ASSERT_TRUE(listed) << listed.error().message;
    for (const auto& entry : *listed) {
        EXPECT_FALSE(entry.key.starts_with(lake::control::workspace_store_search_prefix)) << entry.key;
    }
    const auto refused = liquid::runtime::block_on(reopened->put("search/manifest.json", bytes_of("x")));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::invalid_argument);
}

// lake#535: liquid's one-file commits on the workspace store ask Zetta for one fsync each, lake's
// sync before it answers; liquid's own barriers send nothing, so the writes go without
// durability. After the sync, a crash keeps the commit and LIQUID_JOURNAL stays removed.
TEST(WorkspaceStoreBackend, ACommitAsksForOneDurableRequestAndKeepsItsJournalRemoval) {
    Rig rig;
    const auto root = lake::test::fixture_path("workspace-store-durable-" + rig.workspace);
    const auto identifier = root / "repositories" / "durable";
    std::shared_ptr<ZettaStoreOwner> owner;
    auto store_options = owned(rig, owner);
    auto backend = std::static_pointer_cast<ZettaObjectBackend>(store_options.backend);
    auto store = liquid::runtime::block_on(liquid::store::open(root, std::move(store_options)));
    ASSERT_TRUE(store) << store.error().message;
    liquid::repository_options options;
    options.store = &*store;
    options.checkpoint_interval = 1; // every commit checkpoints and removes its journal
    auto repository = liquid::runtime::block_on(liquid::repository::create(identifier, options));
    ASSERT_TRUE(repository) << repository.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(backend->make_durable()));
    std::optional<liquid::change_id> head;
    for (int commit = 0; commit < 4; ++commit) {
        const std::uint64_t before = rig.zetta->durable_requests(rig.endpoint);
        const std::uint64_t barriers_before = rig.zetta->barriers(rig.endpoint);
        auto change = commit_one_file(*repository, "notes/" + std::to_string(commit) + ".md",
                                      random_bytes(500 + commit * 100, 70 + commit));
        ASSERT_TRUE(change) << change.error().message;
        EXPECT_EQ(rig.zetta->durable_requests(rig.endpoint), before) << "liquid's barriers send no fsync";
        ASSERT_TRUE(liquid::runtime::block_on(backend->make_durable()));
        EXPECT_EQ(rig.zetta->durable_requests(rig.endpoint), before + 1) << "commit " << commit;
        EXPECT_EQ(rig.zetta->barriers(rig.endpoint), barriers_before + 1) << "commit " << commit;
        head = *change;
    }
    // A second sync with nothing new sends nothing.
    const std::uint64_t idle = rig.zetta->durable_requests(rig.endpoint);
    ASSERT_TRUE(liquid::runtime::block_on(backend->make_durable()));
    EXPECT_EQ(rig.zetta->durable_requests(rig.endpoint), idle);
    for (const auto& [key, bytes] : rig.zetta->objects(rig.endpoint)) {
        EXPECT_FALSE(key.ends_with("LIQUID_JOURNAL")) << key;
    }
    owner->stop_renewing();
    rig.zetta->crash(rig.endpoint, 7);
    for (const auto& [key, bytes] : rig.zetta->objects(rig.endpoint)) {
        EXPECT_FALSE(key.ends_with("LIQUID_JOURNAL")) << "the removal was made durable: " << key;
    }
    rig.zetta->expire(rig.endpoint);
    std::shared_ptr<ZettaStoreOwner> successor;
    auto reopened_store = liquid::runtime::block_on(liquid::store::open(root, owned(rig, successor)));
    ASSERT_TRUE(reopened_store) << reopened_store.error().message;
    options.store = &*reopened_store;
    auto reopened = liquid::runtime::block_on(liquid::repository::open(identifier, options));
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto reopened_head = liquid::runtime::block_on(reopened->head());
    ASSERT_TRUE(reopened_head);
    EXPECT_EQ(liquid::to_string(reopened_head->value), liquid::to_string(head->value));
    ASSERT_TRUE(liquid::runtime::block_on(reopened_store->close()));
}

// lake#535: a staged object published under its name and then removed (liquid's object map:
// `objects.meta.tmp`, `objects.meta`, the removal of the staging) sends only the publication,
// and a crash at any point keeps a prefix of the issue order.
TEST(WorkspaceStoreBackend, AStagedObjectRemovedAfterItsPublicationSendsOnlyThePublication) {
    for (std::uint64_t seed = 1; seed <= 16; ++seed) {
        Rig rig;
        auto backend = rig.backend(small_blocks());
        ASSERT_TRUE(liquid::runtime::block_on(backend->put("packs/objects.meta", bytes_of("v1"))));
        ASSERT_TRUE(liquid::runtime::block_on(backend->sync()));
        const std::size_t before = rig.zetta->applied().size();
        ASSERT_TRUE(liquid::runtime::block_on(backend->put("packs/objects.meta.tmp", bytes_of("v2"))));
        ASSERT_TRUE(liquid::runtime::block_on(backend->barrier()));
        ASSERT_TRUE(liquid::runtime::block_on(backend->put("packs/objects.meta", bytes_of("v2"))));
        ASSERT_TRUE(liquid::runtime::block_on(backend->barrier()));
        ASSERT_TRUE(liquid::runtime::block_on(backend->remove("packs/objects.meta.tmp")));
        // Not elided: two held keys after the staging.
        ASSERT_TRUE(liquid::runtime::block_on(backend->put("gc/a.tmp", bytes_of("a"))));
        ASSERT_TRUE(liquid::runtime::block_on(backend->put("gc/b", bytes_of("b"))));
        ASSERT_TRUE(liquid::runtime::block_on(backend->put("gc/c", bytes_of("c"))));
        ASSERT_TRUE(liquid::runtime::block_on(backend->remove("gc/a.tmp")));
        if (seed % 2 == 0) {
            ASSERT_TRUE(liquid::runtime::block_on(backend->sync()));
        }
        const auto applied = rig.zetta->applied();
        std::vector<std::string> sent(applied.begin() + static_cast<std::ptrdiff_t>(before), applied.end());
        EXPECT_EQ(sent, (std::vector<std::string>{"PUT packs/objects.meta", "PUT gc/a.tmp", "PUT gc/b",
                                                  "PUT gc/c", "DELETE gc/a.tmp"}));
        rig.zetta->crash(rig.endpoint, seed);
        const auto kept = rig.zetta->objects(rig.endpoint);
        EXPECT_FALSE(kept.contains("packs/objects.meta.tmp"));
        EXPECT_TRUE(kept.contains("packs/objects.meta"));
        // A prefix of PUT a.tmp, PUT b, PUT c, DELETE a.tmp.
        const std::string state = std::string(kept.contains("gc/a.tmp") ? "a" : "") +
                                  (kept.contains("gc/b") ? "b" : "") + (kept.contains("gc/c") ? "c" : "");
        EXPECT_TRUE(state.empty() || state == "a" || state == "ab" || state == "abc" || state == "bc")
            << "seed " << seed << ": " << state;
        if (seed % 2 == 0) {
            EXPECT_FALSE(kept.contains("gc/a.tmp"));
            EXPECT_TRUE(kept.contains("gc/c"));
        }
    }
}

// lake#535: liquid's close lets go of the backend and revokes the store's grant before it releases
// the owner; the release still syncs what the backend holds, so a crash after a clean close keeps
// everything the store was asked to write.
TEST(WorkspaceStoreOwner, ALiquidCloseSyncsWhatTheBackendHolds) {
    Rig rig;
    const auto root = lake::test::fixture_path("workspace-store-close-" + rig.workspace);
    std::shared_ptr<ZettaStoreOwner> owner;
    std::vector<std::pair<liquid::digest, std::vector<std::byte>>> written;
    {
        auto store = liquid::runtime::block_on(liquid::store::open(root, owned(rig, owner)));
        ASSERT_TRUE(store) << store.error().message;
        for (std::uint64_t seed = 0; seed < 3; ++seed) {
            auto contents = random_bytes(3000 + seed * 7, 500 + seed);
            auto id = liquid::runtime::block_on(store->put(contents));
            ASSERT_TRUE(id) << id.error().message;
            written.emplace_back(*id, std::move(contents));
        }
        const std::uint64_t barriers = rig.zetta->barriers(rig.endpoint);
        ASSERT_TRUE(liquid::runtime::block_on(store->close()));
        EXPECT_EQ(rig.zetta->barriers(rig.endpoint), barriers + 1) << "the release's sync";
    }
    rig.zetta->crash(rig.endpoint, 3);
    std::shared_ptr<ZettaStoreOwner> successor;
    auto store = liquid::runtime::block_on(liquid::store::open(root, owned(rig, successor)));
    ASSERT_TRUE(store) << store.error().message;
    for (const auto& [id, contents] : written) {
        auto read_back = liquid::runtime::block_on(store->get(id));
        ASSERT_TRUE(read_back) << read_back.error().message;
        EXPECT_EQ(*read_back, contents);
    }
    ASSERT_TRUE(liquid::runtime::block_on(store->close()));
}

} // namespace
