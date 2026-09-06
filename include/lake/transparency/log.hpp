#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "lake/transparency/tuf.hpp"

namespace lake::audit {
struct Event;
}

namespace lake::transparency {

inline constexpr std::size_t transparency_tile_width = 1024;

struct LogState {
    std::uint64_t tree_size = 0;
    std::string root_hash;
};

struct LogReplicationBatch {
    LogState base;
    LogState head;
    std::uint64_t source_tree_size = 0;
    std::vector<std::string> attestations;
};

struct AppendResult {
    std::uint64_t first_index = 0;
    std::uint64_t tree_size = 0;
    std::string root_hash;
    std::size_t tiles_published = 0;
    std::size_t hash_tiles_published = 0;
};

struct Tile {
    std::uint64_t index = 0;
    std::size_t width = 0;
    std::string encoded;
    std::string sha256;
};

struct HashTile {
    std::uint32_t level = 0;
    std::uint64_t index = 0;
    std::size_t width = 0;
    std::string encoded;
    std::string sha256;
};

struct InclusionProof {
    std::uint64_t leaf_index = 0;
    std::uint64_t tree_size = 0;
    std::string root_hash;
    std::vector<std::string> audit_path;
};

struct ConsistencyProof {
    std::uint64_t old_tree_size = 0;
    std::uint64_t new_tree_size = 0;
    std::string old_root_hash;
    std::string new_root_hash;
    std::vector<std::string> audit_path;
};

struct IdentityMatch {
    std::uint64_t index = 0;
    std::string profile;
    std::string issuer;
    std::string subject;
    std::string attestation;
};

struct IdentityScan {
    std::string identity;
    std::uint64_t tree_size = 0;
    std::string root_hash;
    std::size_t tiles_scanned = 0;
    std::uint64_t entries_scanned = 0;
    double duration_ms = 0.0;
    std::vector<IdentityMatch> matches;
};

class TiledLog {
public:
    TiledLog(std::filesystem::path store_root, std::string instance_name);

    Result<AppendResult> append(const std::vector<std::string>& attestations);
    Result<AppendResult> append_audit_event(const audit::Event& event);
    Result<Tile> tile(std::uint64_t index, std::size_t width) const;
    Result<HashTile> hash_tile(std::uint32_t level, std::uint64_t index, std::size_t width) const;
    Result<InclusionProof> inclusion_proof(std::uint64_t leaf_index, std::uint64_t tree_size) const;
    Result<ConsistencyProof> consistency_proof(std::uint64_t old_tree_size,
                                               std::uint64_t new_tree_size) const;
    Result<LogState> state() const;
    Result<LogState> state_at(std::uint64_t tree_size) const;
    Result<LogReplicationBatch> replication_batch(std::uint64_t after_tree_size) const;
    Result<AppendResult> apply_replication_batch(const LogReplicationBatch& batch);
    Result<IdentityScan> scan_identity(const std::string& identity,
                                       std::uint64_t tree_size,
                                       const std::string& root_hash) const;

    [[nodiscard]] const std::filesystem::path& directory() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace lake::transparency
