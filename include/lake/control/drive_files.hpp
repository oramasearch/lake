// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <liquid/runtime.hpp>

#include "lake/control/workspace_store.hpp"
#include "lake/control/zetta_control.hpp"

namespace lake::control {

// The bytes of workspace files (docs/api/drives.md#how-file-bytes-are-kept, lake#561): one object
// per uploaded file in the workspace Zetta's `lake_files` bucket, under an opaque key that says
// nothing about the file (`f/<uuid>`). The bucket has no owner lease: each object has one writer
// (the upload that made its key), Zetta orders every append at an exact offset, and an object is
// never rewritten in place, so there is no in-memory directory to protect as liquid's store has.
inline constexpr std::string_view drive_files_bucket = "lake_files";

// A file is sealed in fixed blocks of this many plaintext bytes (the last one shorter), each with
// AES-256-GCM: a stored block is its 12-byte nonce, its ciphertext and its 16-byte tag. Block `n`
// of a file starts at plaintext offset n * drive_file_block and at object offset
// n * (drive_file_block + drive_file_block_overhead), so any byte range is read and opened
// without the rest of the file.
inline constexpr std::uint64_t drive_file_block = 1024 * 1024;
inline constexpr std::uint64_t drive_file_block_overhead = 12 + 16;

// The object bytes a file of `plaintext` bytes takes.
std::uint64_t drive_file_sealed_size(std::uint64_t plaintext);
// Where block `index` starts in the object.
std::uint64_t drive_file_sealed_offset(std::uint64_t index);
// The plaintext bytes an object of `sealed` stored bytes holds: whole sealed blocks, and a last one
// that holds more than its overhead.
std::uint64_t drive_file_plaintext_size(std::uint64_t sealed);
// A new opaque object key.
std::string new_drive_file_key();

// The keys that seal one workspace's files: derived from LAKE_INSTANCE_KEK for that workspace
// alone (HKDF, as workspace records derive theirs), never from the workspace store's data key,
// which lives inside `lake_store` and opens only under that store's lease.
//
// A block's nonce is derived from the block itself: HMAC-SHA-256 under a second derived key of
// (object key, block index, plaintext), its first 12 bytes. The same block always seals to the
// same bytes, so a chunk a client sends again after a lost answer appends exactly what is already
// there, which Zetta's exact-offset append accepts as unchanged; and two different blocks never
// share a nonce under one key. The associated data binds each block to its workspace, object key
// and index, so a block copied to another file, place or workspace does not open.
class DriveFileCipher {
public:
    static ZettaControlResult<DriveFileCipher> open(std::string_view instance_kek,
                                                    std::string workspace_id);

    // Block `index` of the object `key`: plaintext of at most drive_file_block bytes.
    std::string seal(std::string_view key, std::uint64_t index, std::string_view plaintext) const;
    // The plaintext of a stored block; `corrupt` when it does not open here.
    ZettaControlResult<std::string>
    unseal(std::string_view key, std::uint64_t index, std::string_view sealed) const;

private:
    struct Keys;
    DriveFileCipher(std::string workspace_id, std::shared_ptr<const Keys> keys);
    std::string associated(std::string_view key, std::uint64_t index) const;

    std::string workspace_id_;
    std::shared_ptr<const Keys> keys_;
};

// One workspace's `lake_files` bucket, over a ZettaObjectClient for it, and its cipher.
class DriveFileStore {
public:
    DriveFileStore(std::shared_ptr<ZettaObjectClient> client, DriveFileCipher cipher);

    // Creates the bucket (idempotent), at most once per store.
    liquid::runtime::task<ZettaControlResult<void>> ready();
    // Seals `plaintext`, which starts at plaintext `offset` (a multiple of drive_file_block), and
    // appends it durably at its place in the object. Every block but the file's last is whole:
    // `plaintext` is a whole number of blocks unless `last`.
    liquid::runtime::task<ZettaControlResult<void>>
    write(std::string key, std::uint64_t offset, std::string plaintext, bool last);
    // Plaintext [offset, offset + length) of a file of `size` bytes.
    liquid::runtime::task<ZettaControlResult<std::string>>
    read(std::string key, std::uint64_t size, std::uint64_t offset, std::uint64_t length);
    // Reads the whole object back block by block: its plaintext SHA-256 (hex), or `conflict` when
    // the object does not hold exactly `size` bytes of plaintext.
    liquid::runtime::task<ZettaControlResult<std::string>> digest(std::string key,
                                                                  std::uint64_t size);
    liquid::runtime::task<ZettaControlResult<void>> remove(std::string key);
    // How many plaintext bytes the object holds now, from its stored size (lake#588): what an
    // upload has received, whatever chunk was just sent. Zero when there is no object yet.
    liquid::runtime::task<ZettaControlResult<std::uint64_t>> received(std::string key);

private:
    std::shared_ptr<ZettaObjectClient> client_;
    DriveFileCipher cipher_;
    bool ready_ = false;
};

// The SHA-256 of `bytes`, as lowercase hex.
std::string drive_sha256_hex(std::string_view bytes);

} // namespace lake::control
