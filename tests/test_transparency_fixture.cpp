// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include <openssl/evp.h>

#include <nlohmann/json.hpp>

using nlohmann::json;

namespace {

using Bytes = std::vector<unsigned char>;
using Hash = std::array<unsigned char, 32>;

constexpr std::string_view entry_header = "LAKE-TILE-1\n";
constexpr std::string_view hash_header = "LAKE-HASH-TILE-1\n";

Hash digest(std::span<const unsigned char> value) {
    Hash output{};
    unsigned int size = 0;
    EXPECT_EQ(EVP_Digest(
                  value.data(), value.size(), output.data(), &size, EVP_sha256(), nullptr),
              1);
    EXPECT_EQ(size, output.size());
    return output;
}

Hash leaf_hash(std::span<const unsigned char> value) {
    Bytes input;
    input.reserve(value.size() + 1);
    input.push_back(0);
    input.insert(input.end(), value.begin(), value.end());
    return digest(input);
}

Hash node_hash(const Hash& left, const Hash& right) {
    Bytes input;
    input.reserve(65);
    input.push_back(1);
    input.insert(input.end(), left.begin(), left.end());
    input.insert(input.end(), right.begin(), right.end());
    return digest(input);
}

std::string hex(const Hash& value) {
    constexpr std::string_view alphabet = "0123456789abcdef";
    std::string output;
    output.reserve(64);
    for (const unsigned char byte : value) {
        output.push_back(alphabet[byte >> 4]);
        output.push_back(alphabet[byte & 0x0f]);
    }
    return output;
}

bool decode_hex(std::string_view value, Hash& output) {
    if (value.size() != output.size() * 2) {
        return false;
    }
    const auto nibble = [](char digit) -> int {
        if (digit >= '0' && digit <= '9') {
            return digit - '0';
        }
        if (digit >= 'a' && digit <= 'f') {
            return digit - 'a' + 10;
        }
        return -1;
    };
    for (std::size_t index = 0; index < output.size(); ++index) {
        const int high = nibble(value[index * 2]);
        const int low = nibble(value[index * 2 + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        output[index] = static_cast<unsigned char>((high << 4) | low);
    }
    return true;
}

bool decode_hex_bytes(std::string_view value, Bytes& output) {
    if (value.size() % 2 != 0) {
        return false;
    }
    output.clear();
    output.reserve(value.size() / 2);
    for (std::size_t index = 0; index < value.size(); index += 2) {
        const auto nibble = [](char digit) -> int {
            if (digit >= '0' && digit <= '9') {
                return digit - '0';
            }
            if (digit >= 'a' && digit <= 'f') {
                return digit - 'a' + 10;
            }
            return -1;
        };
        const int high = nibble(value[index]);
        const int low = nibble(value[index + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        output.push_back(static_cast<unsigned char>((high << 4) | low));
    }
    return true;
}

bool verifies_ed25519(std::span<const unsigned char> public_key,
                      std::span<const unsigned char> message,
                      std::span<const unsigned char> signature) {
    EVP_PKEY* key = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr, public_key.data(), public_key.size());
    if (key == nullptr) {
        return false;
    }
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (context == nullptr) {
        EVP_PKEY_free(key);
        return false;
    }
    const bool verified = EVP_DigestVerifyInit(context, nullptr, nullptr, nullptr, key) == 1 &&
                          EVP_DigestVerify(context,
                                           signature.data(),
                                           signature.size(),
                                           message.data(),
                                           message.size()) == 1;
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(key);
    return verified;
}

Bytes decode_base64url(std::string value) {
    std::ranges::replace(value, '-', '+');
    std::ranges::replace(value, '_', '/');
    if (value.size() % 4 != 0) {
        value.append(4 - value.size() % 4, '=');
    }
    Bytes output((value.size() / 4) * 3);
    const int decoded = EVP_DecodeBlock(
        output.data(), reinterpret_cast<const unsigned char*>(value.data()), value.size());
    EXPECT_GE(decoded, 0);
    std::size_t size = static_cast<std::size_t>(decoded);
    if (value.ends_with("==")) {
        size -= 2;
    } else if (value.ends_with('=')) {
        --size;
    }
    output.resize(size);
    return output;
}

std::uint32_t read_u32(const Bytes& value, std::size_t offset) {
    return (static_cast<std::uint32_t>(value[offset]) << 24) |
           (static_cast<std::uint32_t>(value[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(value[offset + 2]) << 8) |
           static_cast<std::uint32_t>(value[offset + 3]);
}

bool decode_entry_tile(const Bytes& encoded, std::vector<Bytes>& entries) {
    if (encoded.size() < entry_header.size() ||
        !std::ranges::equal(entry_header,
                            std::string_view(reinterpret_cast<const char*>(encoded.data()),
                                             entry_header.size()))) {
        return false;
    }
    std::size_t offset = entry_header.size();
    while (offset < encoded.size()) {
        if (encoded.size() - offset < 4) {
            return false;
        }
        const std::uint32_t size = read_u32(encoded, offset);
        offset += 4;
        if (size == 0 || size > encoded.size() - offset) {
            return false;
        }
        entries.emplace_back(encoded.begin() + static_cast<std::ptrdiff_t>(offset),
                             encoded.begin() + static_cast<std::ptrdiff_t>(offset + size));
        offset += size;
    }
    return true;
}

bool decode_hash_tile(const Bytes& encoded, std::size_t width, std::vector<Hash>& hashes) {
    if (encoded.size() != hash_header.size() + width * Hash{}.size() ||
        !std::ranges::equal(hash_header,
                            std::string_view(reinterpret_cast<const char*>(encoded.data()),
                                             hash_header.size()))) {
        return false;
    }
    std::size_t offset = hash_header.size();
    for (std::size_t index = 0; index < width; ++index) {
        Hash hash{};
        std::ranges::copy_n(encoded.begin() + static_cast<std::ptrdiff_t>(offset),
                            hash.size(),
                            hash.begin());
        hashes.push_back(hash);
        offset += hash.size();
    }
    return true;
}

std::size_t largest_power_of_two_less_than(std::size_t value) {
    std::size_t power = 1;
    while (power * 2 < value) {
        power *= 2;
    }
    return power;
}

Hash merkle_root(const std::vector<Bytes>& entries, std::size_t first, std::size_t count) {
    if (count == 1) {
        return leaf_hash(entries[first]);
    }
    const std::size_t split = largest_power_of_two_less_than(count);
    return node_hash(merkle_root(entries, first, split),
                     merkle_root(entries, first + split, count - split));
}

bool inclusion_root(const Hash& leaf,
                    std::size_t leaf_index,
                    std::size_t tree_size,
                    const std::vector<Hash>& path,
                    std::size_t& position,
                    Hash& root) {
    if (tree_size == 1) {
        root = leaf;
        return true;
    }
    const std::size_t split = largest_power_of_two_less_than(tree_size);
    Hash child{};
    if (leaf_index < split) {
        if (!inclusion_root(leaf, leaf_index, split, path, position, child) ||
            position == path.size()) {
            return false;
        }
        root = node_hash(child, path[position++]);
        return true;
    }
    if (!inclusion_root(
            leaf, leaf_index - split, tree_size - split, path, position, child) ||
        position == path.size()) {
        return false;
    }
    root = node_hash(path[position++], child);
    return true;
}

bool verifies_inclusion(const Bytes& entry,
                        std::size_t leaf_index,
                        std::size_t tree_size,
                        const std::vector<Hash>& path,
                        const Hash& expected_root) {
    std::size_t position = 0;
    Hash root{};
    return inclusion_root(
               leaf_hash(entry), leaf_index, tree_size, path, position, root) &&
           position == path.size() && root == expected_root;
}

bool verifies_consistency(std::size_t old_size,
                          std::size_t new_size,
                          const Hash& old_root,
                          const Hash& new_root,
                          const std::vector<Hash>& path) {
    if (old_size == 0) {
        return path.empty();
    }
    if (old_size == new_size) {
        return path.empty() && old_root == new_root;
    }
    if (old_size > new_size || path.empty()) {
        return false;
    }
    std::size_t old_node = old_size - 1;
    std::size_t new_node = new_size - 1;
    while ((old_node & 1) != 0) {
        old_node >>= 1;
        new_node >>= 1;
    }
    std::size_t position = 0;
    Hash old_candidate{};
    Hash new_candidate{};
    if (old_node == 0) {
        old_candidate = old_root;
        new_candidate = old_root;
    } else {
        old_candidate = path[position];
        new_candidate = path[position++];
    }
    while (position < path.size()) {
        if (new_node == 0) {
            return false;
        }
        if ((old_node & 1) != 0 || old_node == new_node) {
            old_candidate = node_hash(path[position], old_candidate);
            new_candidate = node_hash(path[position], new_candidate);
            while (old_node != 0 && (old_node & 1) == 0) {
                old_node >>= 1;
                new_node >>= 1;
            }
        } else {
            new_candidate = node_hash(new_candidate, path[position]);
        }
        old_node >>= 1;
        new_node >>= 1;
        ++position;
    }
    return new_node == 0 && old_candidate == old_root && new_candidate == new_root;
}

std::vector<Hash> proof_path(const json& value) {
    std::vector<Hash> path;
    for (const json& encoded : value.at("audit_path")) {
        Hash hash{};
        EXPECT_TRUE(decode_hex(encoded.get<std::string>(), hash));
        path.push_back(hash);
    }
    return path;
}

} // namespace

TEST(TransparencyConformance, FixtureVerifiesIndependentlyAndDetectsTampering) {
    const std::filesystem::path path =
        std::filesystem::path(LAKE_TEST_SOURCE_DIR) / "tests/fixtures/transparency/fixture.json";
    std::ifstream input(path);
    ASSERT_TRUE(input);
    const json fixture = json::parse(input);
    ASSERT_EQ(fixture.at("version"), "lake_transparency_fixture/1");
    const json& checkpoint_record = fixture.at("checkpoint");
    const json& checkpoint = checkpoint_record.at("checkpoint");
    ASSERT_EQ(checkpoint.at("tree_size"), fixture.at("entries").size());
    ASSERT_EQ(checkpoint_record.at("witness_signatures").size(), 2U);
    const std::string checkpoint_encoded = checkpoint.dump();
    const Bytes checkpoint_bytes(checkpoint_encoded.begin(), checkpoint_encoded.end());
    EXPECT_EQ(hex(digest(checkpoint_bytes)),
              checkpoint_record.at("checkpoint_hash_sha256").get<std::string>());
    for (const json& witness : checkpoint_record.at("witness_signatures")) {
        Bytes public_key;
        Bytes signature;
        ASSERT_TRUE(decode_hex_bytes(witness.at("public_key").get<std::string>(), public_key));
        ASSERT_TRUE(decode_hex_bytes(witness.at("signature").get<std::string>(), signature));
        EXPECT_TRUE(verifies_ed25519(public_key, checkpoint_bytes, signature));
    }

    const json& entry_tile_value = fixture.at("tiles").at(0);
    const Bytes entry_tile = decode_base64url(entry_tile_value.at("tile"));
    EXPECT_EQ(hex(digest(entry_tile)), entry_tile_value.at("sha256").get<std::string>());
    std::vector<Bytes> entries;
    ASSERT_TRUE(decode_entry_tile(entry_tile, entries));
    ASSERT_EQ(entries.size(), fixture.at("entries").size());
    for (std::size_t index = 0; index < entries.size(); ++index) {
        EXPECT_EQ(entries[index], decode_base64url(fixture.at("entries").at(index)));
    }
    const Hash root = merkle_root(entries, 0, entries.size());
    EXPECT_EQ(hex(root), checkpoint.at("root_hash").get<std::string>());

    std::vector<std::vector<Hash>> expected_levels(1);
    expected_levels[0].reserve(entries.size());
    for (const Bytes& entry : entries) {
        expected_levels[0].push_back(leaf_hash(entry));
    }
    while (expected_levels.back().size() >= 2) {
        std::vector<Hash> next;
        for (std::size_t index = 0; index + 1 < expected_levels.back().size(); index += 2) {
            next.push_back(
                node_hash(expected_levels.back()[index], expected_levels.back()[index + 1]));
        }
        expected_levels.push_back(std::move(next));
    }
    ASSERT_EQ(fixture.at("tiles").size(), expected_levels.size());
    for (std::size_t level = 1; level < expected_levels.size(); ++level) {
        const json& tile_value = fixture.at("tiles").at(level);
        const Bytes encoded = decode_base64url(tile_value.at("tile"));
        EXPECT_EQ(hex(digest(encoded)), tile_value.at("sha256").get<std::string>());
        std::vector<Hash> hashes;
        ASSERT_TRUE(
            decode_hash_tile(encoded, tile_value.at("hashes").get<std::size_t>(), hashes));
        EXPECT_EQ(hashes, expected_levels[level]);
    }

    const json& inclusion = fixture.at("proofs").at("inclusion");
    ASSERT_EQ(inclusion.size(), entries.size());
    for (std::size_t leaf_index = 0; leaf_index < entries.size(); ++leaf_index) {
        const json& proof = inclusion.at(leaf_index);
        const std::vector<Hash> path_hashes = proof_path(proof);
        EXPECT_EQ(proof.at("checkpoint_hash_sha256"),
                  checkpoint_record.at("checkpoint_hash_sha256"));
        EXPECT_TRUE(
            verifies_inclusion(entries[leaf_index], leaf_index, entries.size(), path_hashes, root));
        for (std::size_t path_index = 0; path_index < path_hashes.size(); ++path_index) {
            for (std::size_t byte = 0; byte < Hash{}.size(); ++byte) {
                std::vector<Hash> changed = path_hashes;
                changed[path_index][byte] ^= 1;
                EXPECT_FALSE(verifies_inclusion(
                    entries[leaf_index], leaf_index, entries.size(), changed, root));
            }
        }
    }

    const json& consistency = fixture.at("proofs").at("consistency");
    Hash old_root{};
    ASSERT_TRUE(decode_hex(consistency.at("old_root_hash").get<std::string>(), old_root));
    const std::vector<Hash> consistency_hashes = proof_path(consistency);
    const std::size_t old_tree_size = consistency.at("old_tree_size").get<std::size_t>();
    const std::size_t new_tree_size = consistency.at("new_tree_size").get<std::size_t>();
    EXPECT_EQ(old_root, merkle_root(entries, 0, old_tree_size));
    EXPECT_TRUE(verifies_consistency(old_tree_size,
                                     new_tree_size,
                                     old_root,
                                     root,
                                     consistency_hashes));
    for (std::size_t path_index = 0; path_index < consistency_hashes.size(); ++path_index) {
        for (std::size_t byte = 0; byte < Hash{}.size(); ++byte) {
            std::vector<Hash> changed = consistency_hashes;
            changed[path_index][byte] ^= 1;
            EXPECT_FALSE(verifies_consistency(old_tree_size,
                                              new_tree_size,
                                              old_root,
                                              root,
                                              changed));
        }
    }

    for (const json& tile_value : fixture.at("tiles")) {
        const Bytes encoded = decode_base64url(tile_value.at("tile"));
        for (std::size_t byte = 0; byte < encoded.size(); ++byte) {
            Bytes changed = encoded;
            changed[byte] ^= 1;
            EXPECT_NE(hex(digest(changed)), tile_value.at("sha256").get<std::string>());
        }
    }
}
