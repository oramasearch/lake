#include "lake/control/drive_files.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <format>
#include <memory>
#include <utility>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "lake/control/agent_bus.hpp"
#include "lake/control/store.hpp"
#include "secrets_crypto.hpp"

namespace lake::control {
namespace {

constexpr std::string_view seal_label = "lake/drive-files/v1/seal/";
constexpr std::string_view nonce_label = "lake/drive-files/v1/nonce/";
constexpr std::size_t nonce_size = 12;
constexpr std::size_t tag_size = 16;

template <class T> ZettaControlResult<T> refused(std::string code, std::string message) {
    return std::unexpected(ZettaControlError{std::move(code), std::move(message)});
}

ZettaControlError object_error(const liquid::error& error) {
    std::string code = "store_unavailable";
    switch (error.code) {
    case liquid::errc::not_found:
        code = "not_found";
        break;
    case liquid::errc::conflict:
    case liquid::errc::corrupt: // 416: a range past the object's end
        code = "conflict";
        break;
    case liquid::errc::busy:
        code = "quota_exceeded";
        break;
    case liquid::errc::unauthorized:
        code = "unauthorized";
        break;
    case liquid::errc::invalid_argument:
        code = "invalid_input";
        break;
    default:
        break;
    }
    return ZettaControlError{std::move(code), error.message};
}

void encode_index(std::string& out, std::uint64_t index) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((index >> shift) & 0xffU));
    }
}

using CipherContext = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

} // namespace

struct DriveFileCipher::Keys {
    secrets_crypto::Key seal{};
    secrets_crypto::Key nonce{};
    ~Keys() {
        OPENSSL_cleanse(seal.data(), seal.size());
        OPENSSL_cleanse(nonce.data(), nonce.size());
    }
};

std::uint64_t drive_file_sealed_offset(std::uint64_t index) {
    return index * (drive_file_block + drive_file_block_overhead);
}

std::uint64_t drive_file_plaintext_size(std::uint64_t sealed) {
    const std::uint64_t whole = sealed / (drive_file_block + drive_file_block_overhead);
    const std::uint64_t rest = sealed % (drive_file_block + drive_file_block_overhead);
    return whole * drive_file_block +
           (rest > drive_file_block_overhead ? rest - drive_file_block_overhead : 0);
}

std::uint64_t drive_file_sealed_size(std::uint64_t plaintext) {
    const std::uint64_t blocks = (plaintext + drive_file_block - 1) / drive_file_block;
    return plaintext + blocks * drive_file_block_overhead;
}

std::string new_drive_file_key() {
    return "f/" + new_uuid_v4();
}

std::string drive_sha256_hex(std::string_view bytes) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    EVP_Digest(bytes.data(), bytes.size(), digest, &size, EVP_sha256(), nullptr);
    std::string out;
    out.reserve(size * 2);
    for (unsigned index = 0; index < size; ++index) {
        out += std::format("{:02x}", digest[index]);
    }
    return out;
}

DriveFileCipher::DriveFileCipher(std::string workspace_id, std::shared_ptr<const Keys> keys)
    : workspace_id_(std::move(workspace_id)), keys_(std::move(keys)) {}

ZettaControlResult<DriveFileCipher> DriveFileCipher::open(std::string_view instance_kek,
                                                          std::string workspace_id) {
    std::ranges::transform(workspace_id, workspace_id.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (workspace_id.empty()) {
        return refused<DriveFileCipher>("invalid_input", "workspace_id must name a workspace");
    }
    auto root = secrets_crypto::instance_key(instance_kek);
    if (!root) {
        return refused<DriveFileCipher>(
            "store_unavailable", "workspace files need LAKE_INSTANCE_KEK: " + root.error().message);
    }
    auto seal = secrets_crypto::derive_hkdf(*root, std::string(seal_label) + workspace_id);
    auto nonce = secrets_crypto::derive_hkdf(*root, std::string(nonce_label) + workspace_id);
    secrets_crypto::cleanse(*root);
    if (!seal || !nonce) {
        return refused<DriveFileCipher>("store_error", "could not derive the workspace file keys");
    }
    auto keys = std::make_shared<Keys>();
    keys->seal = *seal;
    keys->nonce = *nonce;
    secrets_crypto::cleanse(*seal);
    secrets_crypto::cleanse(*nonce);
    return DriveFileCipher(std::move(workspace_id), std::move(keys));
}

std::string DriveFileCipher::associated(std::string_view key, std::uint64_t index) const {
    return std::format("lake-drive-files/v1|{}|{}|{}", workspace_id_, key, index);
}

std::string
DriveFileCipher::seal(std::string_view key, std::uint64_t index, std::string_view plaintext) const {
    // The synthetic nonce: HMAC(nonce key, len(key) ‖ key ‖ index ‖ plaintext), first 12 bytes.
    std::string message;
    message.reserve(16 + key.size() + plaintext.size());
    encode_index(message, key.size());
    message.append(key);
    encode_index(message, index);
    message.append(plaintext);
    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int mac_size = 0;
    HMAC(EVP_sha256(),
         keys_->nonce.data(),
         static_cast<int>(keys_->nonce.size()),
         reinterpret_cast<const unsigned char*>(message.data()),
         message.size(),
         mac,
         &mac_size);
    std::string out(nonce_size + plaintext.size() + tag_size, '\0');
    std::memcpy(out.data(), mac, nonce_size);
    const std::string aad = associated(key, index);
    CipherContext context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    int written = 0;
    int final_written = 0;
    const bool sealed =
        context &&
        EVP_EncryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN, nonce_size, nullptr) == 1 &&
        EVP_EncryptInit_ex(context.get(), nullptr, nullptr, keys_->seal.data(), mac) == 1 &&
        EVP_EncryptUpdate(context.get(),
                          nullptr,
                          &written,
                          reinterpret_cast<const unsigned char*>(aad.data()),
                          static_cast<int>(aad.size())) == 1 &&
        EVP_EncryptUpdate(context.get(),
                          reinterpret_cast<unsigned char*>(out.data() + nonce_size),
                          &written,
                          reinterpret_cast<const unsigned char*>(plaintext.data()),
                          static_cast<int>(plaintext.size())) == 1 &&
        EVP_EncryptFinal_ex(context.get(),
                            reinterpret_cast<unsigned char*>(out.data() + nonce_size + written),
                            &final_written) == 1 &&
        EVP_CIPHER_CTX_ctrl(context.get(),
                            EVP_CTRL_GCM_GET_TAG,
                            tag_size,
                            out.data() + nonce_size + plaintext.size()) == 1;
    OPENSSL_cleanse(message.data(), message.size());
    return sealed ? out : std::string();
}

ZettaControlResult<std::string>
DriveFileCipher::unseal(std::string_view key, std::uint64_t index, std::string_view sealed) const {
    if (sealed.size() < nonce_size + tag_size ||
        sealed.size() > drive_file_block + drive_file_block_overhead) {
        return refused<std::string>("corrupt", "a stored file block is malformed");
    }
    const std::size_t length = sealed.size() - nonce_size - tag_size;
    std::string out(length, '\0');
    std::string tag(sealed.substr(nonce_size + length, tag_size));
    const std::string aad = associated(key, index);
    CipherContext context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    int written = 0;
    int final_written = 0;
    const bool opened =
        context &&
        EVP_DecryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN, nonce_size, nullptr) == 1 &&
        EVP_DecryptInit_ex(context.get(),
                           nullptr,
                           nullptr,
                           keys_->seal.data(),
                           reinterpret_cast<const unsigned char*>(sealed.data())) == 1 &&
        EVP_DecryptUpdate(context.get(),
                          nullptr,
                          &written,
                          reinterpret_cast<const unsigned char*>(aad.data()),
                          static_cast<int>(aad.size())) == 1 &&
        EVP_DecryptUpdate(context.get(),
                          reinterpret_cast<unsigned char*>(out.data()),
                          &written,
                          reinterpret_cast<const unsigned char*>(sealed.data() + nonce_size),
                          static_cast<int>(length)) == 1 &&
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG, tag_size, tag.data()) == 1 &&
        EVP_DecryptFinal_ex(context.get(),
                            reinterpret_cast<unsigned char*>(out.data() + written),
                            &final_written) == 1;
    if (!opened) {
        OPENSSL_cleanse(out.data(), out.size());
        return refused<std::string>("corrupt", "a stored file block does not open");
    }
    return out;
}

DriveFileStore::DriveFileStore(std::shared_ptr<ZettaObjectClient> client, DriveFileCipher cipher)
    : client_(std::move(client)), cipher_(std::move(cipher)) {}

liquid::runtime::task<ZettaControlResult<void>> DriveFileStore::ready() {
    if (ready_) {
        co_return ZettaControlResult<void>();
    }
    auto created = co_await client_->create_bucket(0);
    if (!created) {
        co_return ZettaControlResult<void>(std::unexpected(object_error(created.error())));
    }
    ready_ = true;
    co_return ZettaControlResult<void>();
}

liquid::runtime::task<ZettaControlResult<void>>
DriveFileStore::write(std::string key, std::uint64_t offset, std::string plaintext, bool last) {
    if (offset % drive_file_block != 0) {
        co_return ZettaControlResult<void>(std::unexpected(ZettaControlError{
            "invalid_input", "a chunk starts at a multiple of 1 MiB", "/offset"}));
    }
    if (!last && plaintext.size() % drive_file_block != 0) {
        co_return ZettaControlResult<void>(std::unexpected(ZettaControlError{
            "invalid_input", "every chunk but the last is a whole number of MiB", "/content"}));
    }
    if (plaintext.empty()) {
        co_return ZettaControlResult<void>();
    }
    auto bucket = settled(co_await ready());
    if (!bucket) {
        co_return ZettaControlResult<void>(std::unexpected(bucket.error()));
    }
    const std::uint64_t first = offset / drive_file_block;
    std::string sealed;
    sealed.reserve(drive_file_sealed_size(plaintext.size()));
    for (std::uint64_t at = 0; at < plaintext.size(); at += drive_file_block) {
        const std::string_view block = std::string_view(plaintext).substr(
            at, std::min<std::uint64_t>(drive_file_block, plaintext.size() - at));
        const std::string piece = cipher_.seal(key, first + at / drive_file_block, block);
        if (piece.empty()) {
            co_return ZettaControlResult<void>(
                std::unexpected(ZettaControlError{"crypto_error", "could not seal a file block"}));
        }
        sealed += piece;
    }
    OPENSSL_cleanse(plaintext.data(), plaintext.size());
    // Durable: Zetta fsyncs before it answers, so an acknowledged chunk survives a crash.
    auto appended = co_await client_->append(
        key, drive_file_sealed_offset(first), std::move(sealed), std::nullopt, true);
    if (!appended) {
        co_return ZettaControlResult<void>(std::unexpected(object_error(appended.error())));
    }
    co_return ZettaControlResult<void>();
}

liquid::runtime::task<ZettaControlResult<std::string>> DriveFileStore::read(std::string key,
                                                                            std::uint64_t size,
                                                                            std::uint64_t offset,
                                                                            std::uint64_t length) {
    if (offset > size) {
        co_return refused<std::string>("invalid_input", "the range starts past the file's end");
    }
    length = std::min(length, size - offset);
    std::string out;
    if (length == 0) {
        co_return ZettaControlResult<std::string>(std::move(out));
    }
    out.reserve(length);
    const std::uint64_t first = offset / drive_file_block;
    const std::uint64_t last = (offset + length - 1) / drive_file_block;
    // One ranged read for every block the range touches.
    const std::uint64_t end_plain = std::min(size, (last + 1) * drive_file_block);
    const std::uint64_t sealed_begin = drive_file_sealed_offset(first);
    const std::uint64_t sealed_end = drive_file_sealed_offset(last) +
                                     (end_plain - last * drive_file_block) +
                                     drive_file_block_overhead;
    auto bytes = co_await client_->get(key, sealed_begin, sealed_end - sealed_begin);
    if (!bytes) {
        co_return ZettaControlResult<std::string>(std::unexpected(object_error(bytes.error())));
    }
    if (bytes->size() != sealed_end - sealed_begin) {
        co_return refused<std::string>("conflict", "the file's object is shorter than its size");
    }
    std::size_t at = 0;
    for (std::uint64_t index = first; index <= last; ++index) {
        const std::uint64_t plain = std::min(drive_file_block, size - index * drive_file_block);
        const std::size_t stored = plain + drive_file_block_overhead;
        auto opened = cipher_.unseal(key, index, std::string_view(*bytes).substr(at, stored));
        if (!opened) {
            co_return ZettaControlResult<std::string>(std::unexpected(opened.error()));
        }
        at += stored;
        const std::uint64_t block_start = index * drive_file_block;
        const std::uint64_t from = std::max(offset, block_start) - block_start;
        const std::uint64_t to = std::min(offset + length, block_start + plain) - block_start;
        out.append(*opened, from, to - from);
        OPENSSL_cleanse(opened->data(), opened->size());
    }
    co_return ZettaControlResult<std::string>(std::move(out));
}

liquid::runtime::task<ZettaControlResult<std::string>> DriveFileStore::digest(std::string key,
                                                                              std::uint64_t size) {
    // The object holds exactly the sealed bytes of `size`: its last byte is there and nothing
    // follows it. Checked first, so a short or long object is `conflict`, never a block that
    // does not open.
    const std::uint64_t sealed = drive_file_sealed_size(size);
    if (size > 0) {
        auto last = co_await client_->get(key, sealed - 1, 1);
        if (!last) {
            const auto error = object_error(last.error());
            co_return refused<std::string>(
                error.code == "conflict" || error.code == "not_found" ? "conflict" : error.code,
                error.code == "conflict" || error.code == "not_found"
                    ? "the file's object is shorter than its size"
                    : error.message);
        }
    }
    auto beyond = co_await client_->get(key, sealed, 1);
    if (beyond && !beyond->empty()) {
        co_return refused<std::string>("conflict", "the file's object is longer than its size");
    }
    if (!beyond && beyond.error().code != liquid::errc::not_found &&
        beyond.error().code != liquid::errc::corrupt) {
        co_return ZettaControlResult<std::string>(std::unexpected(object_error(beyond.error())));
    }
    using Digest = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    Digest context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
        co_return refused<std::string>("crypto_error", "could not start a SHA-256");
    }
    // Eight blocks per read: a bounded buffer whatever the file's size.
    constexpr std::uint64_t window = 8 * drive_file_block;
    for (std::uint64_t offset = 0; offset < size; offset += window) {
        auto plain = settled(co_await read(key, size, offset, window));
        if (!plain) {
            co_return ZettaControlResult<std::string>(std::unexpected(plain.error()));
        }
        EVP_DigestUpdate(context.get(), plain->data(), plain->size());
        OPENSSL_cleanse(plain->data(), plain->size());
    }
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_size = 0;
    EVP_DigestFinal_ex(context.get(), digest, &digest_size);
    std::string out;
    for (unsigned index = 0; index < digest_size; ++index) {
        out += std::format("{:02x}", digest[index]);
    }
    co_return ZettaControlResult<std::string>(std::move(out));
}

liquid::runtime::task<ZettaControlResult<std::uint64_t>> DriveFileStore::received(std::string key) {
    auto listed = co_await client_->list(key);
    if (!listed) {
        if (listed.error().code == liquid::errc::not_found) {
            co_return ZettaControlResult<std::uint64_t>(std::uint64_t{0});
        }
        co_return ZettaControlResult<std::uint64_t>(std::unexpected(object_error(listed.error())));
    }
    for (const ObjectListEntry& object : *listed) {
        if (object.key == key) {
            co_return ZettaControlResult<std::uint64_t>(drive_file_plaintext_size(object.size));
        }
    }
    co_return ZettaControlResult<std::uint64_t>(std::uint64_t{0});
}

liquid::runtime::task<ZettaControlResult<void>> DriveFileStore::remove(std::string key) {
    auto removed = co_await client_->remove(key, std::nullopt, true);
    if (!removed && removed.error().code != liquid::errc::not_found) {
        co_return ZettaControlResult<void>(std::unexpected(object_error(removed.error())));
    }
    co_return ZettaControlResult<void>();
}

} // namespace lake::control
