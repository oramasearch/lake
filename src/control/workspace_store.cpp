#include "lake/control/workspace_store.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <format>
#include <span>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <zetta/connect/http_transport.hpp>

#include "secrets_crypto.hpp"

namespace lake::control {
namespace {

constexpr std::size_t header_size = 48;
constexpr std::array<char, 8> header_magic{'L', 'K', 'S', 'T', 'E', 'N', 'C', '1'};
constexpr std::uint32_t header_format = 1;
constexpr std::string_view tail_marker = ".lake-tail-";
// One Zetta append frame carries at most 4 MiB; a PATCH no larger is applied as one frame, so it
// is absent or whole after a crash and an object never ends inside a block.
constexpr std::size_t zetta_slice_bytes = 4 * 1024 * 1024;
constexpr std::string_view wrap_label = "lake/workspace-store/v1/";

liquid::error failure(liquid::errc code, std::string message, std::string context = {}) {
    return liquid::error{code, std::move(message), std::move(context)};
}

template <class T = void> liquid::result<T> refused(liquid::errc code, std::string message,
                                                    std::string context = {}) {
    return std::unexpected(failure(code, std::move(message), std::move(context)));
}

std::string encode_component(std::string_view input, bool keep_slash) {
    constexpr std::string_view hex = "0123456789ABCDEF";
    std::string output;
    output.reserve(input.size());
    for (const unsigned char character : input) {
        if (std::isalnum(character) || character == '-' || character == '_' || character == '.' ||
            character == '~' || (keep_slash && character == '/')) {
            output += static_cast<char>(character);
            continue;
        }
        output += '%';
        output += hex[character >> 4U];
        output += hex[character & 0x0fU];
    }
    return output;
}

std::string hex_of(std::span<const unsigned char> bytes) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string output;
    output.reserve(bytes.size() * 2);
    for (const unsigned char value : bytes) {
        output += digits[value >> 4U];
        output += digits[value & 0x0fU];
    }
    return output;
}

void put_u32(std::string& output, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        output += static_cast<char>((value >> shift) & 0xffU);
    }
}

void put_u64(std::string& output, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        output += static_cast<char>((value >> shift) & 0xffU);
    }
}

std::uint32_t get_u32(std::string_view bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (int index = 3; index >= 0; --index) {
        value = (value << 8U) | static_cast<unsigned char>(bytes[offset + index]);
    }
    return value;
}

std::uint64_t get_u64(std::string_view bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (int index = 7; index >= 0; --index) {
        value = (value << 8U) | static_cast<unsigned char>(bytes[offset + index]);
    }
    return value;
}

// The error envelope Zetta answers with: {"ok": false, "error": {"code", "message", "details"}}.
struct ZettaError {
    std::string code;
    std::string message;
    nlohmann::json details;
};

ZettaError zetta_error(const ObjectResponse& response) {
    const nlohmann::json body = nlohmann::json::parse(response.body, nullptr, false);
    ZettaError output;
    if (body.is_object() && body.contains("error") && body.at("error").is_object()) {
        const nlohmann::json& error = body.at("error");
        output.code = error.value("code", std::string());
        output.message = error.value("message", std::string());
        output.details = error.value("details", nlohmann::json());
    }
    if (output.message.empty()) {
        output.message = "HTTP " + std::to_string(response.status);
    }
    return output;
}

liquid::error mapped(const ObjectResponse& response, std::string context) {
    if (!response.error.empty()) {
        return failure(liquid::errc::unavailable,
                       "the workspace Zetta did not answer: " + response.error, std::move(context));
    }
    const ZettaError error = zetta_error(response);
    switch (response.status) {
    case 400:
        return failure(liquid::errc::invalid_argument, error.message, std::move(context));
    case 401:
    case 403:
        return failure(liquid::errc::unauthorized, error.message, std::move(context));
    case 404:
        return failure(liquid::errc::not_found, error.message, std::move(context));
    case 409:
        if (error.code == "fenced") {
            return failure(liquid::errc::locked,
                           "the workspace store's lease has passed to another holder (Zetta "
                           "refused the write as fenced): " + error.message,
                           std::move(context));
        }
        if (error.code == "held") {
            return failure(liquid::errc::locked, error.message, std::move(context));
        }
        return failure(liquid::errc::conflict, error.message, std::move(context));
    case 416:
        return failure(liquid::errc::corrupt, error.message, std::move(context));
    case 503:
        return failure(liquid::errc::unavailable, error.message, std::move(context));
    case 507:
        return failure(liquid::errc::busy, "the workspace store's quota is reached: " + error.message,
                       std::move(context));
    default:
        return failure(response.status >= 500 ? liquid::errc::unavailable : liquid::errc::io,
                       error.message, std::move(context));
    }
}

// The `value` of a successful JSON envelope.
liquid::result<nlohmann::json> envelope_value(const ObjectResponse& response, std::string context) {
    const nlohmann::json body = nlohmann::json::parse(response.body, nullptr, false);
    if (!body.is_object() || !body.value("ok", false)) {
        return std::unexpected(failure(liquid::errc::corrupt,
                                       "the workspace Zetta answered without a JSON envelope",
                                       std::move(context)));
    }
    return body.value("value", nlohmann::json::object());
}

class ZettaHttpObjectTransport final : public ObjectTransport {
public:
    liquid::runtime::task<ObjectResponse> exchange(ObjectRequest input) override {
        const zetta::connect::AsyncHttpTransport& transport =
            zetta::connect::default_async_http_transport();
        if (!transport) {
            co_return ObjectResponse{0, {}, "Zetta HTTP transport is not installed"};
        }
        zetta::connect::HttpRequest request;
        request.method = std::move(input.method);
        request.url = std::move(input.url);
        request.headers = std::move(input.headers);
        request.body = std::move(input.body);
        request.timeout_seconds = input.timeout_seconds;
        request.secret_headers = {"Authorization"};
        auto [transport_error, response] = co_await transport(request);
        if (transport_error) {
            co_return ObjectResponse{0, {}, transport_error.message()};
        }
        co_return ObjectResponse{response.status, std::move(response.body),
                                 std::move(response.error)};
    }
};

} // namespace

std::shared_ptr<ObjectTransport> make_zetta_object_transport() {
    return std::make_shared<ZettaHttpObjectTransport>();
}

// ---- ZettaObjectClient -----------------------------------------------------------------------

ZettaObjectClient::ZettaObjectClient(std::shared_ptr<ObjectTransport> transport,
                                     ObjectEndpoint endpoint,
                                     std::string bucket,
                                     std::shared_ptr<ObjectStoreCounters> counters)
    : transport_(std::move(transport)), endpoint_(std::move(endpoint)), bucket_(std::move(bucket)),
      counters_(counters ? std::move(counters) : std::make_shared<ObjectStoreCounters>()) {
    while (endpoint_.url.ends_with('/')) {
        endpoint_.url.pop_back();
    }
}

std::string ZettaObjectClient::object_url(std::string_view key) const {
    return endpoint_.url + "/v1/objects/" + bucket_ + "/" + encode_component(key, true);
}

std::string ZettaObjectClient::bucket_url(std::string_view suffix) const {
    return endpoint_.url + "/v1/objects/" + bucket_ + std::string(suffix);
}

liquid::runtime::task<ObjectResponse> ZettaObjectClient::send(ObjectRequest request,
                                                              bool idempotent) {
    request.headers.emplace_back("Authorization", "Bearer " + endpoint_.token);
    const int attempts = idempotent ? 4 : 1;
    ObjectResponse response;
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        const auto started = std::chrono::steady_clock::now();
        auto answered = co_await transport_->exchange(request);
        if (!answered) {
            response = ObjectResponse{0, {}, answered.error().message};
        } else {
            response = std::move(*answered);
        }
        counters_->requests.fetch_add(1);
        counters_->request_nanoseconds.fetch_add(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - started)
                .count()));
        counters_->bytes_sent.fetch_add(request.body.size());
        counters_->bytes_received.fetch_add(response.body.size());
        const bool retry = !response.error.empty() || response.status == 503;
        if (!retry || attempt == attempts) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10 * attempt * attempt));
    }
    co_return response;
}

liquid::runtime::task<void> ZettaObjectClient::create_bucket(std::uint64_t quota_bytes) {
    nlohmann::json input{{"bucket", bucket_}};
    if (quota_bytes != 0) {
        input["quota_bytes"] = quota_bytes;
    }
    ObjectRequest request;
    request.method = "POST";
    request.url = endpoint_.url + "/v1/create_bucket";
    request.headers = {{"Content-Type", "application/json"}, {"Accept", "application/json"}};
    request.body = input.dump();
    auto response = co_await send(std::move(request), true);
    if (!response) {
        co_return std::unexpected(response.error());
    }
    // A bucket that exists with another definition (another quota) is used as it is.
    if (response->status == 409 && response->error.empty() &&
        zetta_error(*response).code == "conflict") {
        co_return liquid::result<void>{};
    }
    if (response->status != 200 && response->status != 201) {
        co_return std::unexpected(mapped(*response, "zetta bucket " + bucket_));
    }
    co_return liquid::result<void>{};
}

liquid::runtime::task<void> ZettaObjectClient::put(std::string key,
                                                   std::string bytes,
                                                   std::optional<std::uint64_t> fence,
                                                   bool if_absent) {
    counters_->puts.fetch_add(1);
    ObjectRequest request;
    request.method = "PUT";
    request.url = object_url(key) + (if_absent ? "?if_absent=true" : "");
    request.headers = {{"Content-Type", "application/octet-stream"}};
    if (fence) {
        request.headers.emplace_back("Zetta-Fence", std::to_string(*fence));
    }
    request.body = std::move(bytes);
    auto response = co_await send(std::move(request), !if_absent);
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (response->status == 200 || response->status == 201) {
        co_return liquid::result<void>{};
    }
    auto error = mapped(*response, bucket_ + "/" + key);
    if (if_absent && error.code == liquid::errc::conflict) {
        error.code = liquid::errc::already_exists;
    }
    co_return std::unexpected(std::move(error));
}

liquid::runtime::task<void> ZettaObjectClient::append(std::string key,
                                                      std::uint64_t offset,
                                                      std::string bytes,
                                                      std::optional<std::uint64_t> fence) {
    counters_->appends.fetch_add(1);
    ObjectRequest request;
    request.method = "PATCH";
    request.url = object_url(key) + "?offset=" + std::to_string(offset);
    request.headers = {{"Content-Type", "application/octet-stream"}};
    if (fence) {
        request.headers.emplace_back("Zetta-Fence", std::to_string(*fence));
    }
    request.body = std::move(bytes);
    // A retried append of the same bytes at the same offset changes nothing.
    auto response = co_await send(std::move(request), true);
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (response->status == 200 || response->status == 201) {
        co_return liquid::result<void>{};
    }
    co_return std::unexpected(mapped(*response, bucket_ + "/" + key));
}

liquid::runtime::task<std::string> ZettaObjectClient::get(std::string key,
                                                          std::uint64_t offset,
                                                          std::uint64_t size) {
    if (size == 0) {
        co_return std::string();
    }
    counters_->gets.fetch_add(1);
    ObjectRequest request;
    request.method = "GET";
    request.url = object_url(key);
    request.headers = {{"Range", std::format("bytes={}-{}", offset, offset + size - 1)}};
    auto response = co_await send(std::move(request), true);
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (response->status != 200 && response->status != 206) {
        co_return std::unexpected(mapped(*response, bucket_ + "/" + key));
    }
    if (response->body.size() != size) {
        co_return std::unexpected(failure(
            liquid::errc::corrupt,
            std::format("the workspace Zetta answered {} bytes for a range of {}",
                        response->body.size(), size),
            bucket_ + "/" + key));
    }
    co_return std::move(response->body);
}

liquid::runtime::task<std::string> ZettaObjectClient::get_all(std::string key) {
    counters_->gets.fetch_add(1);
    ObjectRequest request;
    request.method = "GET";
    request.url = object_url(key);
    auto response = co_await send(std::move(request), true);
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (response->status != 200 && response->status != 206) {
        co_return std::unexpected(mapped(*response, bucket_ + "/" + key));
    }
    co_return std::move(response->body);
}

liquid::runtime::task<void> ZettaObjectClient::remove(std::string key,
                                                      std::optional<std::uint64_t> fence) {
    counters_->removes.fetch_add(1);
    ObjectRequest request;
    request.method = "DELETE";
    request.url = object_url(key);
    if (fence) {
        request.headers.emplace_back("Zetta-Fence", std::to_string(*fence));
    }
    auto response = co_await send(std::move(request), true);
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (response->status == 200 || response->status == 204) {
        co_return liquid::result<void>{};
    }
    co_return std::unexpected(mapped(*response, bucket_ + "/" + key));
}

liquid::runtime::task<std::vector<ObjectListEntry>> ZettaObjectClient::list(std::string prefix) {
    std::vector<ObjectListEntry> output;
    std::string after;
    for (;;) {
        counters_->lists.fetch_add(1);
        ObjectRequest request;
        request.method = "GET";
        request.url = bucket_url("") + "?prefix=" + encode_component(prefix, false) + "&limit=1000";
        if (!after.empty()) {
            request.url += "&after=" + encode_component(after, false);
        }
        request.headers = {{"Accept", "application/json"}};
        auto response = co_await send(std::move(request), true);
        if (!response) {
            co_return std::unexpected(response.error());
        }
        if (response->status != 200) {
            co_return std::unexpected(mapped(*response, bucket_ + "/" + prefix));
        }
        auto value = envelope_value(*response, bucket_ + "/" + prefix);
        if (!value) {
            co_return std::unexpected(value.error());
        }
        for (const nlohmann::json& object : value->value("objects", nlohmann::json::array())) {
            output.push_back({object.value("key", std::string()), object.value("size", std::uint64_t{0})});
        }
        after = value->value("next_after", std::string());
        if (after.empty()) {
            break;
        }
    }
    co_return output;
}

liquid::runtime::task<void> ZettaObjectClient::barrier() {
    counters_->barriers.fetch_add(1);
    ObjectRequest request;
    request.method = "POST";
    request.url = bucket_url(":barrier");
    request.headers = {{"Content-Type", "application/json"}, {"Accept", "application/json"}};
    request.body = "{}";
    auto response = co_await send(std::move(request), true);
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (response->status != 200) {
        co_return std::unexpected(mapped(*response, bucket_ + ":barrier"));
    }
    co_return liquid::result<void>{};
}

liquid::runtime::task<ObjectUsage> ZettaObjectClient::usage(std::chrono::milliseconds timeout) {
    ObjectRequest request;
    request.method = "GET";
    request.url = bucket_url(":usage");
    request.headers = {{"Accept", "application/json"}};
    request.timeout_seconds = std::max<int>(1, static_cast<int>(timeout.count() / 1000));
    auto response = co_await send(std::move(request), false);
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (response->status != 200) {
        co_return std::unexpected(mapped(*response, bucket_ + ":usage"));
    }
    auto value = envelope_value(*response, bucket_ + ":usage");
    if (!value) {
        co_return std::unexpected(value.error());
    }
    co_return ObjectUsage{value->value("objects", std::uint64_t{0}),
                          value->value("bytes", std::uint64_t{0}),
                          value->value("quota_bytes", std::uint64_t{0})};
}

namespace {

liquid::result<ObjectLease> lease_of(const ObjectResponse& response, std::string context) {
    auto value = envelope_value(response, context);
    if (!value) {
        return std::unexpected(value.error());
    }
    return ObjectLease{value->value("holder", std::string()), value->value("fence", std::uint64_t{0}),
                       value->value("expires_ms", std::int64_t{0})};
}

} // namespace

liquid::runtime::task<ObjectLease> ZettaObjectClient::acquire(std::string holder,
                                                              std::chrono::milliseconds ttl,
                                                              std::chrono::milliseconds wait) {
    ObjectRequest request;
    request.method = "POST";
    request.url = bucket_url(":acquire");
    request.headers = {{"Content-Type", "application/json"}, {"Accept", "application/json"}};
    request.body =
        nlohmann::json{{"holder", holder}, {"ttl_ms", ttl.count()}, {"wait_ms", wait.count()}}.dump();
    request.timeout_seconds = static_cast<int>(wait.count() / 1000) + 30;
    auto response = co_await send(std::move(request), false);
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (response->status != 200) {
        auto error = mapped(*response, bucket_ + ":acquire");
        if (error.code == liquid::errc::locked) {
            const ZettaError details = zetta_error(*response);
            std::string current = details.details.is_object()
                                      ? details.details.value("holder", std::string())
                                      : std::string();
            error.message = "the workspace store is owned by " +
                            (current.empty() ? std::string("another holder") : current) +
                            "; one lake process owns a workspace store at a time";
        }
        co_return std::unexpected(std::move(error));
    }
    co_return lease_of(*response, bucket_ + ":acquire");
}

liquid::runtime::task<ObjectLease> ZettaObjectClient::renew(std::string holder,
                                                            std::uint64_t fence,
                                                            std::chrono::milliseconds ttl,
                                                            std::chrono::milliseconds timeout) {
    ObjectRequest request;
    request.method = "POST";
    request.url = bucket_url(":renew");
    request.headers = {{"Content-Type", "application/json"}, {"Accept", "application/json"}};
    request.body =
        nlohmann::json{{"holder", holder}, {"fence", fence}, {"ttl_ms", ttl.count()}}.dump();
    // One attempt, answered well inside the lease: a renewal that cannot finish in time is worth
    // nothing, and the owner's next renewal is the retry.
    request.timeout_seconds = std::max<int>(1, static_cast<int>(timeout.count() / 1000));
    auto response = co_await send(std::move(request), false);
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (response->status != 200) {
        co_return std::unexpected(mapped(*response, bucket_ + ":renew"));
    }
    co_return lease_of(*response, bucket_ + ":renew");
}

liquid::runtime::task<void> ZettaObjectClient::release(std::string holder, std::uint64_t fence) {
    ObjectRequest request;
    request.method = "POST";
    request.url = bucket_url(":release");
    request.headers = {{"Content-Type", "application/json"}, {"Accept", "application/json"}};
    request.body = nlohmann::json{{"holder", holder}, {"fence", fence}}.dump();
    auto response = co_await send(std::move(request), true);
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (response->status != 200) {
        co_return std::unexpected(mapped(*response, bucket_ + ":release"));
    }
    co_return liquid::result<void>{};
}

// ---- WorkspaceStoreCipher --------------------------------------------------------------------

namespace {

struct CipherContext {
    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    ~CipherContext() {
        EVP_CIPHER_CTX_free(context);
    }
};

liquid::result<std::string> gcm_seal(const WorkspaceStoreCipher::Key& key,
                                     std::string_view associated,
                                     std::span<const std::byte> plain) {
    std::array<unsigned char, WorkspaceStoreCipher::nonce_size> nonce{};
    if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
        return refused<std::string>(liquid::errc::internal, "could not generate a nonce");
    }
    CipherContext cipher;
    if (cipher.context == nullptr ||
        EVP_EncryptInit_ex(cipher.context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(cipher.context, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce.size()),
                            nullptr) != 1 ||
        EVP_EncryptInit_ex(cipher.context, nullptr, nullptr, key.data(), nonce.data()) != 1) {
        return refused<std::string>(liquid::errc::internal, "could not start AES-256-GCM");
    }
    int length = 0;
    if (!associated.empty() &&
        EVP_EncryptUpdate(cipher.context, nullptr, &length,
                          reinterpret_cast<const unsigned char*>(associated.data()),
                          static_cast<int>(associated.size())) != 1) {
        return refused<std::string>(liquid::errc::internal, "could not authenticate a block");
    }
    std::string output(WorkspaceStoreCipher::nonce_size + plain.size() + WorkspaceStoreCipher::tag_size,
                       '\0');
    std::memcpy(output.data(), nonce.data(), nonce.size());
    auto* out = reinterpret_cast<unsigned char*>(output.data() + nonce.size());
    if (!plain.empty() &&
        EVP_EncryptUpdate(cipher.context, out, &length,
                          reinterpret_cast<const unsigned char*>(plain.data()),
                          static_cast<int>(plain.size())) != 1) {
        return refused<std::string>(liquid::errc::internal, "could not encrypt a block");
    }
    int final_length = 0;
    if (EVP_EncryptFinal_ex(cipher.context, out + plain.size(), &final_length) != 1 ||
        EVP_CIPHER_CTX_ctrl(cipher.context, EVP_CTRL_GCM_GET_TAG,
                            static_cast<int>(WorkspaceStoreCipher::tag_size),
                            out + plain.size()) != 1) {
        return refused<std::string>(liquid::errc::internal, "could not finish a block");
    }
    return output;
}

liquid::result<std::vector<std::byte>> gcm_open(const WorkspaceStoreCipher::Key& key,
                                                std::string_view associated,
                                                std::string_view sealed) {
    if (sealed.size() < WorkspaceStoreCipher::overhead) {
        return refused<std::vector<std::byte>>(liquid::errc::corrupt,
                                               "an encrypted block is truncated");
    }
    const auto* nonce = reinterpret_cast<const unsigned char*>(sealed.data());
    const std::size_t size = sealed.size() - WorkspaceStoreCipher::overhead;
    const auto* body = nonce + WorkspaceStoreCipher::nonce_size;
    const auto* tag = body + size;
    CipherContext cipher;
    if (cipher.context == nullptr ||
        EVP_DecryptInit_ex(cipher.context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(cipher.context, EVP_CTRL_GCM_SET_IVLEN,
                            static_cast<int>(WorkspaceStoreCipher::nonce_size), nullptr) != 1 ||
        EVP_DecryptInit_ex(cipher.context, nullptr, nullptr, key.data(), nonce) != 1) {
        return refused<std::vector<std::byte>>(liquid::errc::internal, "could not start AES-256-GCM");
    }
    int length = 0;
    if (!associated.empty() &&
        EVP_DecryptUpdate(cipher.context, nullptr, &length,
                          reinterpret_cast<const unsigned char*>(associated.data()),
                          static_cast<int>(associated.size())) != 1) {
        return refused<std::vector<std::byte>>(liquid::errc::internal, "could not authenticate a block");
    }
    std::vector<std::byte> output(size);
    if (size != 0 &&
        EVP_DecryptUpdate(cipher.context, reinterpret_cast<unsigned char*>(output.data()), &length,
                          body, static_cast<int>(size)) != 1) {
        return refused<std::vector<std::byte>>(liquid::errc::corrupt, "an encrypted block does not decrypt");
    }
    if (EVP_CIPHER_CTX_ctrl(cipher.context, EVP_CTRL_GCM_SET_TAG,
                            static_cast<int>(WorkspaceStoreCipher::tag_size),
                            const_cast<unsigned char*>(tag)) != 1) {
        return refused<std::vector<std::byte>>(liquid::errc::internal, "could not set a block's tag");
    }
    int final_length = 0;
    if (EVP_DecryptFinal_ex(cipher.context, reinterpret_cast<unsigned char*>(output.data()) + size,
                            &final_length) != 1) {
        return refused<std::vector<std::byte>>(
            liquid::errc::corrupt,
            "an encrypted block failed authentication: it was altered, moved, or written under "
            "another workspace's key");
    }
    return output;
}

std::string key_associated(std::string_view workspace_id, std::uint32_t version) {
    return std::string(wrap_label) + std::string(workspace_id) + "/data-key/" +
           std::to_string(version);
}

} // namespace

WorkspaceStoreCipher::WorkspaceStoreCipher(std::string workspace_id,
                                           std::uint32_t key_version,
                                           Key data_key)
    : workspace_id_(std::move(workspace_id)), key_version_(key_version), key_(data_key) {}

WorkspaceStoreCipher::~WorkspaceStoreCipher() {
    secrets_crypto::cleanse(key_);
}

liquid::result<WorkspaceStoreCipher::Key>
WorkspaceStoreCipher::wrapping_key(std::string_view instance_kek, std::string_view workspace_id) {
    if (workspace_id.empty()) {
        return refused<Key>(liquid::errc::invalid_argument, "a workspace store needs a workspace id");
    }
    auto root = secrets_crypto::instance_key(instance_kek);
    if (!root) {
        return refused<Key>(liquid::errc::not_configured,
                            "a workspace store needs LAKE_INSTANCE_KEK: " + root.error().message);
    }
    auto derived =
        secrets_crypto::derive_hkdf(*root, std::string(wrap_label) + std::string(workspace_id));
    secrets_crypto::cleanse(*root);
    if (!derived) {
        return refused<Key>(liquid::errc::internal, derived.error().message);
    }
    return *derived;
}

liquid::result<std::string> WorkspaceStoreCipher::wrap(const Key& wrapping,
                                                       std::string_view workspace_id,
                                                       std::uint32_t version,
                                                       const Key& key) {
    auto sealed = gcm_seal(wrapping, key_associated(workspace_id, version),
                           std::as_bytes(std::span(key.data(), key.size())));
    if (!sealed) {
        return std::unexpected(sealed.error());
    }
    auto encoded = secrets_crypto::base64url_encode(*sealed);
    if (!encoded) {
        return refused<std::string>(liquid::errc::internal, encoded.error().message);
    }
    return nlohmann::json{{"format", "lake-workspace-store-key/1"},
                          {"workspace_id", std::string(workspace_id)},
                          {"version", version},
                          {"algorithm", "AES-256-GCM"},
                          {"wrapped", *encoded}}
        .dump();
}

liquid::result<std::pair<std::uint32_t, WorkspaceStoreCipher::Key>>
WorkspaceStoreCipher::unwrap(const Key& wrapping,
                             std::string_view workspace_id,
                             std::string_view document) {
    using Output = std::pair<std::uint32_t, Key>;
    const nlohmann::json value = nlohmann::json::parse(document, nullptr, false);
    if (!value.is_object() || value.value("format", std::string()) != "lake-workspace-store-key/1" ||
        !value.contains("version") || !value.at("version").is_number_unsigned() ||
        !value.contains("wrapped") || !value.at("wrapped").is_string()) {
        return refused<Output>(liquid::errc::corrupt, "the workspace store key document is malformed",
                               std::string(workspace_store_key_object));
    }
    if (value.value("workspace_id", std::string()) != workspace_id) {
        return refused<Output>(liquid::errc::unauthorized,
                               "the workspace store key belongs to another workspace",
                               std::string(workspace_store_key_object));
    }
    const auto version = value.at("version").get<std::uint32_t>();
    auto sealed = secrets_crypto::base64url_decode(value.at("wrapped").get<std::string>());
    if (!sealed) {
        return refused<Output>(liquid::errc::corrupt, "the workspace store key is not base64url",
                               std::string(workspace_store_key_object));
    }
    auto opened = gcm_open(wrapping, key_associated(workspace_id, version), *sealed);
    if (!opened || opened->size() != 32) {
        return refused<Output>(liquid::errc::unauthorized,
                               "the workspace store key does not unwrap under this lake's "
                               "LAKE_INSTANCE_KEK",
                               std::string(workspace_store_key_object));
    }
    Key key{};
    std::memcpy(key.data(), opened->data(), key.size());
    std::fill(opened->begin(), opened->end(), std::byte{0});
    return Output{version, key};
}

liquid::result<std::string> WorkspaceStoreCipher::seal(std::string_view associated,
                                                       std::span<const std::byte> plain) const {
    return gcm_seal(key_, associated, plain);
}

liquid::result<std::vector<std::byte>> WorkspaceStoreCipher::open(std::string_view associated,
                                                                  std::string_view sealed) const {
    return gcm_open(key_, associated, sealed);
}

// ---- ZettaObjectBackend ----------------------------------------------------------------------

struct ZettaObjectBackend::Shared {
    std::shared_ptr<ZettaObjectClient> client;
    ZettaObjectBackendOptions options;

    std::mutex cipher_mutex;
    std::shared_ptr<const WorkspaceStoreCipher> cipher;
    std::atomic<bool> failed{false};

    // Aligned ciphertext windows: "<key>\n<salt>\n<window index>" -> bytes (the window's bytes as
    // far as the object reached when it was read). A version's bytes never change: an object only
    // gains complete blocks, and a replacement has a new salt.
    std::mutex cache_mutex;
    std::list<std::string> lru;
    std::map<std::string, std::pair<std::string, std::list<std::string>::iterator>> windows;
    std::size_t cached_bytes = 0;

    // [from, to) of a cached window that reaches at least `to`, or nothing.
    std::optional<std::string> slice(const std::string& name, std::size_t from, std::size_t to) {
        std::lock_guard lock(cache_mutex);
        const auto found = windows.find(name);
        if (found == windows.end() || found->second.first.size() < to) {
            return std::nullopt;
        }
        lru.splice(lru.begin(), lru, found->second.second);
        return found->second.first.substr(from, to - from);
    }

    void remember(const std::string& name, std::string bytes) {
        std::lock_guard lock(cache_mutex);
        if (bytes.size() > options.cache_bytes) {
            return;
        }
        if (const auto found = windows.find(name); found != windows.end()) {
            cached_bytes -= found->second.first.size();
            lru.erase(found->second.second);
            windows.erase(found);
        }
        while (!lru.empty() && cached_bytes + bytes.size() > options.cache_bytes) {
            const auto victim = windows.find(lru.back());
            cached_bytes -= victim->second.first.size();
            windows.erase(victim);
            lru.pop_back();
        }
        lru.push_front(name);
        cached_bytes += bytes.size();
        windows.emplace(name, std::make_pair(std::move(bytes), lru.begin()));
    }

    // Bytes this process just appended at `offset` of an object version: they extend its cached
    // windows, so reading back what was just written costs no request.
    void extend(const std::string& key, const std::string& salt, std::uint64_t offset,
                std::string_view bytes) {
        const std::uint64_t window = options.window_bytes;
        while (!bytes.empty()) {
            const std::uint64_t index = offset / window;
            const std::uint64_t start = index * window;
            const std::size_t take =
                static_cast<std::size_t>(std::min<std::uint64_t>(bytes.size(), start + window - offset));
            const std::string name = key + "\n" + salt + "\n" + std::to_string(index);
            {
                std::lock_guard lock(cache_mutex);
                const auto found = windows.find(name);
                if (found != windows.end() && found->second.first.size() == offset - start &&
                    cached_bytes + take <= options.cache_bytes) {
                    found->second.first.append(bytes.substr(0, take));
                    cached_bytes += take;
                    lru.splice(lru.begin(), lru, found->second.second);
                    offset += take;
                    bytes.remove_prefix(take);
                    continue;
                }
            }
            if (offset == start) {
                remember(name, std::string(bytes.substr(0, take)));
            }
            offset += take;
            bytes.remove_prefix(take);
        }
    }

    void forget(const std::string& key) {
        std::lock_guard lock(cache_mutex);
        const std::string prefix = key + "\n";
        for (auto it = windows.lower_bound(prefix);
             it != windows.end() && it->first.starts_with(prefix);) {
            cached_bytes -= it->second.first.size();
            lru.erase(it->second.second);
            it = windows.erase(it);
        }
    }
};

struct ZettaObjectBackend::Entry {
    // The object K in Zetta and its ciphertext size there.
    bool remote = false;
    std::uint64_t ct_size = 0;
    // The header, once known: always for an object this view created or replaced.
    std::optional<std::string> header;
    std::uint64_t base_len = 0;
    std::uint32_t block_size = 0;
    std::string salt_hex;
    // Tail objects in Zetta for K: key -> ciphertext size.
    std::map<std::string, std::uint64_t> tails;
    // The open region: the plaintext after the last complete block in Zetta. `open_remote` of its
    // first bytes are held by the current tail object; the rest are buffered appends.
    bool open_loaded = false;
    std::vector<std::byte> open;
    std::uint64_t open_remote = 0;

    std::uint64_t base_ct() const {
        if (base_len == 0) {
            return 0;
        }
        const std::uint64_t blocks = (base_len + block_size - 1) / block_size;
        return base_len + blocks * WorkspaceStoreCipher::overhead;
    }
    std::uint64_t block_ct() const {
        return block_size + WorkspaceStoreCipher::overhead;
    }
    // Complete appended blocks in Zetta.
    std::uint64_t append_blocks() const {
        if (!remote || !header) {
            return 0;
        }
        return (ct_size - header_size - base_ct()) / block_ct();
    }
    std::string tail_key(const std::string& key, std::uint64_t block) const {
        return key + std::string(tail_marker) + std::to_string(block) + "-" + salt_hex;
    }
    std::optional<std::uint64_t> valid_tail_plain(const std::string& key) const {
        const auto found = tails.find(tail_key(key, append_blocks()));
        if (found == tails.end() || found->second < WorkspaceStoreCipher::overhead) {
            return std::nullopt;
        }
        return found->second - WorkspaceStoreCipher::overhead;
    }
    std::uint64_t open_size(const std::string& key) const {
        if (open_loaded) {
            return open.size();
        }
        return valid_tail_plain(key).value_or(0);
    }
    std::uint64_t size(const std::string& key) const {
        return base_len + append_blocks() * block_size + open_size(key);
    }
};

namespace {

liquid::result<void> check_liquid_key(std::string_view key) {
    if (key.empty() || key.starts_with('/') || key.size() > 1000) {
        return refused(liquid::errc::invalid_argument, "invalid store backend key", std::string(key));
    }
    if (key.starts_with("keys/") || key.find(tail_marker) != std::string_view::npos) {
        return refused(liquid::errc::invalid_argument,
                       "the key is reserved by lake's workspace store encryption", std::string(key));
    }
    return {};
}

std::string block_associated(const std::string& workspace_id,
                             std::string_view key,
                             std::string_view header,
                             char region,
                             std::uint64_t index,
                             std::size_t length) {
    std::string output = "lake-store/v1";
    output += '\0';
    output += workspace_id;
    output += '\0';
    output += key;
    output += '\0';
    output += header;
    output += region;
    put_u64(output, index);
    put_u64(output, length);
    return output;
}

struct ParsedHeader {
    std::uint32_t key_version = 0;
    std::uint32_t block_size = 0;
    std::uint64_t base_len = 0;
    std::string salt_hex;
};

liquid::result<ParsedHeader> parse_header(std::string_view bytes, const std::string& key) {
    if (bytes.size() != header_size ||
        !std::equal(header_magic.begin(), header_magic.end(), bytes.begin())) {
        return refused<ParsedHeader>(liquid::errc::corrupt,
                                     "the object is not a lake workspace store object", key);
    }
    if (get_u32(bytes, 8) != header_format) {
        return refused<ParsedHeader>(liquid::errc::unsupported,
                                     "unknown workspace store object format", key);
    }
    ParsedHeader output;
    output.key_version = get_u32(bytes, 12);
    output.block_size = get_u32(bytes, 16);
    output.base_len = get_u64(bytes, 24);
    output.salt_hex = hex_of(std::span(reinterpret_cast<const unsigned char*>(bytes.data()) + 32, 8));
    if (output.block_size == 0 || output.block_size > 16U * 1024U * 1024U) {
        return refused<ParsedHeader>(liquid::errc::corrupt, "invalid workspace store block size",
                                     key);
    }
    return output;
}

liquid::result<std::string> make_header(std::uint32_t key_version,
                                        std::uint32_t block_size,
                                        std::uint64_t base_len) {
    std::string output(header_magic.begin(), header_magic.end());
    put_u32(output, header_format);
    put_u32(output, key_version);
    put_u32(output, block_size);
    put_u32(output, 0);
    put_u64(output, base_len);
    std::array<unsigned char, 16> salt{};
    if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1) {
        return refused<std::string>(liquid::errc::internal, "could not generate an object salt");
    }
    output.append(reinterpret_cast<const char*>(salt.data()), salt.size());
    return output;
}

void apply_header(ZettaObjectBackend::Entry& entry, std::string header, const ParsedHeader& parsed) {
    entry.header = std::move(header);
    entry.base_len = parsed.base_len;
    entry.block_size = parsed.block_size;
    entry.salt_hex = parsed.salt_hex;
}
} // namespace

ZettaObjectBackend::ZettaObjectBackend(std::shared_ptr<ZettaObjectClient> client,
                                       ZettaObjectBackendOptions options)
    : shared_(std::make_shared<Shared>()) {
    shared_->client = std::move(client);
    shared_->options = std::move(options);
}

ZettaObjectBackend::ZettaObjectBackend(std::shared_ptr<Shared> shared,
                                       std::optional<std::uint64_t> fence)
    : shared_(std::move(shared)), fence_(fence) {}

ZettaObjectBackend::~ZettaObjectBackend() = default;

const ZettaObjectClient& ZettaObjectBackend::client() const {
    return *shared_->client;
}

liquid::result<std::shared_ptr<liquid::store_backend>>
ZettaObjectBackend::fenced(std::uint64_t fence) const {
    return std::shared_ptr<liquid::store_backend>(std::make_shared<ZettaObjectBackend>(shared_, fence));
}

liquid::runtime::task<void> ZettaObjectBackend::ensure_directory() {
    {
        std::lock_guard lock(mutex_);
        if (directory_loaded_) {
            co_return liquid::result<void>{};
        }
    }
    auto listed = co_await shared_->client->list("");
    if (!listed) {
        co_return std::unexpected(listed.error());
    }
    std::lock_guard lock(mutex_);
    if (directory_loaded_) {
        co_return liquid::result<void>{};
    }
    std::map<std::string, std::uint64_t> orphans;
    for (const ObjectListEntry& object : *listed) {
        if (object.key == workspace_store_key_object || object.key.starts_with("keys/")) {
            continue;
        }
        const std::size_t marker = object.key.find(tail_marker);
        if (marker != std::string::npos) {
            orphans.emplace(object.key, object.size);
            continue;
        }
        auto& entry = directory_[object.key];
        if (!entry) {
            entry = std::make_unique<Entry>();
        }
        entry->remote = true;
        entry->ct_size = object.size;
    }
    for (const auto& [tail, size] : orphans) {
        const std::string owner = tail.substr(0, tail.find(tail_marker));
        const auto found = directory_.find(owner);
        if (found == directory_.end()) {
            stale_tails_.insert(tail);
            continue;
        }
        found->second->tails.emplace(tail, size);
    }
    directory_loaded_ = true;
    co_return liquid::result<void>{};
}

liquid::runtime::task<void> ZettaObjectBackend::ensure_cipher() {
    {
        std::lock_guard lock(shared_->cipher_mutex);
        if (shared_->cipher) {
            co_return liquid::result<void>{};
        }
    }
    // The directory decides whether a missing key may be created: only in an empty bucket.
    if (auto ready = co_await ensure_directory(); !ready) {
        co_return std::unexpected(ready.error());
    }
    const std::string& workspace_id = shared_->options.workspace_id;
    auto wrapping = WorkspaceStoreCipher::wrapping_key(shared_->options.instance_kek, workspace_id);
    if (!wrapping) {
        co_return std::unexpected(wrapping.error());
    }
    // The wrapping key is wiped on every path out of this function.
    struct Wipe {
        WorkspaceStoreCipher::Key& key;
        ~Wipe() {
            secrets_crypto::cleanse(key);
        }
    } wipe{*wrapping};
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto document = co_await shared_->client->get_all(std::string(workspace_store_key_object));
        if (document) {
            auto unwrapped = WorkspaceStoreCipher::unwrap(*wrapping, workspace_id, *document);
            if (!unwrapped) {
                co_return std::unexpected(unwrapped.error());
            }
            std::lock_guard lock(shared_->cipher_mutex);
            if (!shared_->cipher) {
                shared_->cipher = std::make_shared<const WorkspaceStoreCipher>(
                    workspace_id, unwrapped->first, unwrapped->second);
            }
            secrets_crypto::cleanse(unwrapped->second);
            co_return liquid::result<void>{};
        }
        if (document.error().code != liquid::errc::not_found) {
            co_return std::unexpected(document.error());
        }
        // A missing key is created only for a store that is being created: one the caller opened
        // to create (before its workspace is switched) whose bucket holds nothing yet. A switched
        // workspace's store, or a bucket that holds objects, without its key was wiped, recreated
        // or is the wrong bucket; serving it as an empty store would lose its repositories.
        bool empty = false;
        {
            std::lock_guard lock(mutex_);
            empty = directory_.empty() && stale_tails_.empty();
        }
        if (!shared_->options.create_key || !empty) {
            co_return std::unexpected(failure(
                liquid::errc::corrupt,
                !shared_->options.create_key
                    ? "the workspace store has no data key: its workspace was switched to it, so "
                      "the store was wiped, recreated, or this is the wrong workspace Zetta; lake "
                      "will not serve it as an empty store"
                    : "the workspace store has no data key but holds objects: it was damaged or "
                      "this is the wrong workspace Zetta; lake will not create a new key over it",
                std::string(workspace_store_key_object)));
        }
        auto key = secrets_crypto::random_key();
        if (!key) {
            co_return std::unexpected(failure(liquid::errc::internal, key.error().message));
        }
        auto wrapped = WorkspaceStoreCipher::wrap(*wrapping, workspace_id, 1, *key);
        secrets_crypto::cleanse(*key);
        if (!wrapped) {
            co_return std::unexpected(wrapped.error());
        }
        auto created = co_await shared_->client->put(std::string(workspace_store_key_object),
                                                     std::move(*wrapped), fence_, true);
        if (!created && created.error().code != liquid::errc::already_exists) {
            co_return std::unexpected(created.error());
        }
        // Read back what won, ours or a concurrent creator's.
    }
    co_return std::unexpected(failure(liquid::errc::internal,
                                      "the workspace store key could not be created",
                                      std::string(workspace_store_key_object)));
}

liquid::runtime::task<void> ZettaObjectBackend::ensure_header(const std::string& key) {
    std::uint64_t ct_size = 0;
    {
        std::lock_guard lock(mutex_);
        const auto found = directory_.find(key);
        if (found == directory_.end() || found->second->header || !found->second->remote) {
            co_return liquid::result<void>{};
        }
        ct_size = found->second->ct_size;
    }
    if (ct_size < header_size) {
        co_return std::unexpected(
            failure(liquid::errc::corrupt, "the object is shorter than its header", key));
    }
    // The first window holds the header and the first blocks: one read serves both.
    const std::uint64_t window =
        std::min<std::uint64_t>(ct_size, shared_->options.window_bytes);
    auto bytes = co_await shared_->client->get(key, 0, window);
    if (!bytes) {
        co_return std::unexpected(bytes.error());
    }
    std::string header = bytes->substr(0, header_size);
    auto parsed = parse_header(header, key);
    if (!parsed) {
        co_return std::unexpected(parsed.error());
    }
    Entry probe;
    probe.remote = true;
    probe.ct_size = ct_size;
    apply_header(probe, header, *parsed);
    if (ct_size < header_size + probe.base_ct() ||
        (ct_size - header_size - probe.base_ct()) % probe.block_ct() != 0) {
        co_return std::unexpected(failure(liquid::errc::corrupt,
                                          "the object ends inside an encrypted block", key));
    }
    shared_->remember(key + "\n" + parsed->salt_hex + "\n0", std::move(*bytes));
    std::lock_guard lock(mutex_);
    const auto found = directory_.find(key);
    if (found != directory_.end() && !found->second->header && found->second->ct_size == ct_size) {
        Entry& entry = *found->second;
        apply_header(entry, std::move(header), *parsed);
        // Only the tail of the current block can be live; any other was left by an interrupted
        // barrier or an older version of the object and is removed after the next barrier.
        const std::string current = entry.tail_key(key, entry.append_blocks());
        for (auto it = entry.tails.begin(); it != entry.tails.end();) {
            if (it->first != current) {
                stale_tails_.insert(it->first);
                it = entry.tails.erase(it);
            } else {
                ++it;
            }
        }
    }
    co_return liquid::result<void>{};
}

liquid::runtime::task<std::string>
ZettaObjectBackend::read_main(const std::string& key, std::uint64_t offset, std::uint64_t size) {
    std::string salt;
    std::uint64_t ct_size = 0;
    {
        std::lock_guard lock(mutex_);
        const auto found = directory_.find(key);
        if (found == directory_.end() || !found->second->remote) {
            co_return std::unexpected(failure(liquid::errc::not_found, "no such object", key));
        }
        salt = found->second->salt_hex;
        ct_size = found->second->ct_size;
    }
    if (offset + size > ct_size) {
        co_return std::unexpected(
            failure(liquid::errc::corrupt, "an encrypted range is outside the object", key));
    }
    const std::uint64_t window = shared_->options.window_bytes;
    std::string output;
    output.reserve(size);
    for (std::uint64_t index = offset / window; index * window < offset + size; ++index) {
        const std::uint64_t start = index * window;
        const std::uint64_t end = std::min(start + window, ct_size);
        const std::uint64_t needed = std::min(offset + size, end);
        const std::uint64_t from = std::max(offset, start) - start;
        const std::string name = key + "\n" + salt + "\n" + std::to_string(index);
        if (auto hit = shared_->slice(name, from, needed - start)) {
            shared_->client->counters()->window_hits.fetch_add(1);
            output += *hit;
            continue;
        }
        shared_->client->counters()->window_misses.fetch_add(1);
        auto bytes = co_await shared_->client->get(key, start, end - start);
        if (!bytes) {
            co_return std::unexpected(bytes.error());
        }
        output.append(*bytes, from, needed - start - from);
        shared_->remember(name, std::move(*bytes));
    }
    co_return output;
}

liquid::runtime::task<void> ZettaObjectBackend::ensure_open_region(const std::string& key) {
    std::string tail;
    std::uint64_t tail_size = 0;
    std::uint64_t block = 0;
    std::string header;
    {
        std::lock_guard lock(mutex_);
        const auto found = directory_.find(key);
        if (found == directory_.end() || found->second->open_loaded) {
            co_return liquid::result<void>{};
        }
        Entry& entry = *found->second;
        if (!entry.remote || !entry.header) {
            entry.open_loaded = true;
            co_return liquid::result<void>{};
        }
        block = entry.append_blocks();
        tail = entry.tail_key(key, block);
        const auto present = entry.tails.find(tail);
        if (present == entry.tails.end()) {
            entry.open_loaded = true;
            entry.open.clear();
            entry.open_remote = 0;
            co_return liquid::result<void>{};
        }
        tail_size = present->second;
        header = *entry.header;
    }
    auto sealed = co_await shared_->client->get(tail, 0, tail_size);
    if (!sealed) {
        co_return std::unexpected(sealed.error());
    }
    std::shared_ptr<const WorkspaceStoreCipher> cipher;
    {
        std::lock_guard lock(shared_->cipher_mutex);
        cipher = shared_->cipher;
    }
    auto plain = cipher->open(block_associated(shared_->options.workspace_id, key, header, 't',
                                               block, tail_size - WorkspaceStoreCipher::overhead),
                              *sealed);
    if (!plain) {
        co_return std::unexpected(failure(plain.error().code, plain.error().message, tail));
    }
    std::lock_guard lock(mutex_);
    const auto found = directory_.find(key);
    if (found != directory_.end() && !found->second->open_loaded &&
        found->second->append_blocks() == block) {
        found->second->open = std::move(*plain);
        found->second->open_remote = found->second->open.size();
        found->second->open_loaded = true;
    }
    co_return liquid::result<void>{};
}

liquid::runtime::task<std::vector<std::byte>>
ZettaObjectBackend::read_plain(const std::string& key, std::uint64_t offset, std::uint64_t size) {
    std::shared_ptr<const WorkspaceStoreCipher> cipher;
    {
        std::lock_guard lock(shared_->cipher_mutex);
        cipher = shared_->cipher;
    }
    const std::string& workspace_id = shared_->options.workspace_id;
    // A flush can move bytes from the open region into complete blocks between the snapshot and
    // the read; the read then starts again from a new snapshot.
    for (int attempt = 0; attempt < 8; ++attempt) {
        std::uint64_t base_len = 0;
        std::uint64_t block_size = 0;
        std::uint64_t base_ct = 0;
        std::uint64_t blocks = 0;
        std::string header;
        {
            std::lock_guard lock(mutex_);
            const auto found = directory_.find(key);
            if (found == directory_.end()) {
                co_return std::unexpected(failure(liquid::errc::not_found, "no such object", key));
            }
            const Entry& entry = *found->second;
            base_len = entry.base_len;
            block_size = entry.block_size;
            base_ct = entry.base_ct();
            blocks = entry.append_blocks();
            header = entry.header.value_or(std::string());
        }
        std::vector<std::byte> output(size);
        const std::uint64_t end = offset + size;
        std::uint64_t position = offset;
        const std::uint64_t block_ct = block_size + WorkspaceStoreCipher::overhead;
        // The bytes K was created with.
        if (position < end && position < base_len) {
            const std::uint64_t stop = std::min(end, base_len);
            const std::uint64_t first = position / block_size;
            const std::uint64_t last = (stop - 1) / block_size;
            const std::uint64_t last_len = std::min(block_size, base_len - last * block_size);
            const std::uint64_t from = header_size + first * block_ct;
            const std::uint64_t to = header_size + last * block_ct + last_len + WorkspaceStoreCipher::overhead;
            auto sealed = co_await read_main(key, from, to - from);
            if (!sealed) {
                co_return std::unexpected(sealed.error());
            }
            for (std::uint64_t index = first; index <= last; ++index) {
                const std::uint64_t length = std::min(block_size, base_len - index * block_size);
                auto plain = cipher->open(
                    block_associated(workspace_id, key, header, 'b', index, length),
                    std::string_view(*sealed).substr((index - first) * block_ct,
                                                     length + WorkspaceStoreCipher::overhead));
                if (!plain) {
                    co_return std::unexpected(failure(plain.error().code, plain.error().message, key));
                }
                const std::uint64_t block_start = index * block_size;
                const std::uint64_t copy_from = std::max(position, block_start);
                const std::uint64_t copy_to = std::min(stop, block_start + length);
                std::copy_n(plain->begin() + static_cast<std::ptrdiff_t>(copy_from - block_start),
                            copy_to - copy_from,
                            output.begin() + static_cast<std::ptrdiff_t>(copy_from - offset));
            }
            position = stop;
        }
        // Complete appended blocks.
        const std::uint64_t appended_end = base_len + blocks * block_size;
        if (position < end && position < appended_end) {
            const std::uint64_t stop = std::min(end, appended_end);
            const std::uint64_t first = (position - base_len) / block_size;
            const std::uint64_t last = (stop - 1 - base_len) / block_size;
            const std::uint64_t from = header_size + base_ct + first * block_ct;
            auto sealed = co_await read_main(key, from, (last - first + 1) * block_ct);
            if (!sealed) {
                co_return std::unexpected(sealed.error());
            }
            for (std::uint64_t index = first; index <= last; ++index) {
                auto plain = cipher->open(
                    block_associated(workspace_id, key, header, 'a', index, block_size),
                    std::string_view(*sealed).substr((index - first) * block_ct, block_ct));
                if (!plain) {
                    co_return std::unexpected(failure(plain.error().code, plain.error().message, key));
                }
                const std::uint64_t block_start = base_len + index * block_size;
                const std::uint64_t copy_from = std::max(position, block_start);
                const std::uint64_t copy_to = std::min(stop, block_start + block_size);
                std::copy_n(plain->begin() + static_cast<std::ptrdiff_t>(copy_from - block_start),
                            copy_to - copy_from,
                            output.begin() + static_cast<std::ptrdiff_t>(copy_from - offset));
            }
            position = stop;
        }
        if (position >= end) {
            co_return output;
        }
        // The open region: the current tail and buffered appends.
        auto opened = co_await ensure_open_region(key);
        if (!opened) {
            co_return std::unexpected(opened.error());
        }
        std::lock_guard lock(mutex_);
        const auto found = directory_.find(key);
        if (found == directory_.end()) {
            co_return std::unexpected(failure(liquid::errc::not_found, "no such object", key));
        }
        const Entry& entry = *found->second;
        if (entry.append_blocks() != blocks || entry.base_len != base_len) {
            continue;
        }
        if (end - appended_end > entry.open.size()) {
            co_return std::unexpected(
                failure(liquid::errc::corrupt, "a range is outside the object", key));
        }
        std::copy_n(entry.open.begin() + static_cast<std::ptrdiff_t>(position - appended_end),
                    end - position,
                    output.begin() + static_cast<std::ptrdiff_t>(position - offset));
        co_return output;
    }
    co_return std::unexpected(
        failure(liquid::errc::busy, "the object kept changing while it was read", key));
}

liquid::runtime::task<void> ZettaObjectBackend::flush_entry(const std::string& key, bool barrier) {
    // One flush per key at a time: a threshold flush and a barrier on the same key would otherwise
    // send the same blocks twice and both remove them from the buffer.
    for (;;) {
        {
            std::lock_guard lock(mutex_);
            if (!flushing_.contains(key)) {
                flushing_.insert(key);
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    struct Done {
        ZettaObjectBackend& self;
        const std::string& key;
        ~Done() {
            std::lock_guard lock(self.mutex_);
            self.flushing_.erase(key);
        }
    } done{*this, key};
    co_return co_await flush_entry_once(key, barrier);
}

liquid::runtime::task<void> ZettaObjectBackend::flush_entry_once(const std::string& key, bool barrier) {
    std::vector<std::string> patches;
    std::uint64_t first_offset = 0;
    std::optional<std::pair<std::string, std::string>> tail;
    std::uint64_t full = 0;
    std::uint64_t remainder = 0;
    {
        std::shared_ptr<const WorkspaceStoreCipher> cipher;
        {
            std::lock_guard lock(shared_->cipher_mutex);
            cipher = shared_->cipher;
        }
        std::lock_guard lock(mutex_);
        const auto found = directory_.find(key);
        if (found == directory_.end()) {
            co_return liquid::result<void>{};
        }
        Entry& entry = *found->second;
        if (!entry.open_loaded || !entry.header) {
            co_return liquid::result<void>{};
        }
        const std::uint64_t block_size = entry.block_size;
        full = entry.open.size() / block_size;
        remainder = entry.open.size() - full * block_size;
        const bool create = !entry.remote;
        if (!barrier && full * block_size < shared_->options.flush_bytes) {
            co_return liquid::result<void>{};
        }
        if (!create && full == 0 && (!barrier || remainder == entry.open_remote)) {
            co_return liquid::result<void>{};
        }
        const std::uint64_t blocks = entry.append_blocks();
        std::string payload = create ? *entry.header : std::string();
        first_offset = create ? 0 : entry.ct_size;
        for (std::uint64_t index = 0; index < full; ++index) {
            const auto begin = entry.open.begin() + static_cast<std::ptrdiff_t>(index * block_size);
            auto sealed = cipher->seal(
                block_associated(shared_->options.workspace_id, key, *entry.header, 'a',
                                 blocks + index, block_size),
                std::span<const std::byte>(&*begin, block_size));
            if (!sealed) {
                co_return std::unexpected(sealed.error());
            }
            if (!payload.empty() && payload.size() + sealed->size() > zetta_slice_bytes) {
                patches.push_back(std::move(payload));
                payload.clear();
            }
            payload += *sealed;
        }
        if (!payload.empty()) {
            patches.push_back(std::move(payload));
        }
        if (barrier && remainder > 0 && (full > 0 || remainder != entry.open_remote)) {
            const auto begin = entry.open.begin() + static_cast<std::ptrdiff_t>(full * block_size);
            auto sealed = cipher->seal(
                block_associated(shared_->options.workspace_id, key, *entry.header, 't',
                                 blocks + full, remainder),
                std::span<const std::byte>(&*begin, remainder));
            if (!sealed) {
                co_return std::unexpected(sealed.error());
            }
            tail.emplace(entry.tail_key(key, blocks + full), std::move(*sealed));
        }
    }
    std::uint64_t offset = first_offset;
    std::string salt;
    {
        std::lock_guard lock(mutex_);
        const auto found = directory_.find(key);
        salt = found == directory_.end() ? std::string() : found->second->salt_hex;
    }
    for (std::string& patch : patches) {
        const std::uint64_t size = patch.size();
        auto written = co_await shared_->client->append(key, offset, patch, fence_);
        if (!written) {
            co_return std::unexpected(written.error());
        }
        shared_->extend(key, salt, offset, patch);
        offset += size;
        std::lock_guard lock(mutex_);
        dirty_ = true;
        // Record each piece as it lands, so a failure part way leaves the directory exact.
        const auto found = directory_.find(key);
        if (found != directory_.end()) {
            Entry& entry = *found->second;
            const std::uint64_t had = entry.remote ? entry.append_blocks() : 0;
            entry.remote = true;
            entry.ct_size = offset;
            const std::uint64_t gained = entry.append_blocks() - had;
            entry.open.erase(entry.open.begin(),
                             entry.open.begin() + static_cast<std::ptrdiff_t>(gained * entry.block_size));
            if (gained > 0) {
                entry.open_remote = 0;
            }
        }
    }
    if (tail) {
        const std::uint64_t size = tail->second.size();
        auto written = co_await shared_->client->put(tail->first, std::move(tail->second), fence_, false);
        if (!written) {
            co_return std::unexpected(written.error());
        }
        std::lock_guard lock(mutex_);
        dirty_ = true;
        const auto found = directory_.find(key);
        if (found != directory_.end()) {
            found->second->tails[tail->first] = size;
            found->second->open_remote = remainder;
        }
    }
    // Every tail but the current one is stale now.
    std::lock_guard lock(mutex_);
    const auto found = directory_.find(key);
    if (found != directory_.end()) {
        Entry& entry = *found->second;
        const std::string current = entry.tail_key(key, entry.append_blocks());
        for (auto it = entry.tails.begin(); it != entry.tails.end();) {
            if (it->first != current || entry.open_remote == 0) {
                stale_tails_.insert(it->first);
                it = entry.tails.erase(it);
            } else {
                ++it;
            }
        }
    }
    co_return liquid::result<void>{};
}

liquid::runtime::task<void> ZettaObjectBackend::remove_stale_tails() {
    std::set<std::string> stale;
    {
        std::lock_guard lock(mutex_);
        stale.swap(stale_tails_);
    }
    for (const std::string& tail : stale) {
        auto removed = co_await shared_->client->remove(tail, fence_);
        if (!removed && removed.error().code != liquid::errc::not_found) {
            if (removed.error().code == liquid::errc::locked) {
                co_return std::unexpected(removed.error());
            }
            // Ignored by every reader; the next barrier tries again.
            std::lock_guard lock(mutex_);
            stale_tails_.insert(tail);
        }
    }
    co_return liquid::result<void>{};
}

liquid::runtime::task<void> ZettaObjectBackend::put_impl(std::string key, std::vector<std::byte> bytes) {
    if (auto checked = check_liquid_key(key); !checked) {
        co_return std::unexpected(checked.error());
    }
    if (auto ready = co_await ensure_directory(); !ready) {
        co_return std::unexpected(ready.error());
    }
    if (auto ready = co_await ensure_cipher(); !ready) {
        co_return std::unexpected(ready.error());
    }
    std::shared_ptr<const WorkspaceStoreCipher> cipher;
    {
        std::lock_guard lock(shared_->cipher_mutex);
        cipher = shared_->cipher;
    }
    const auto block_size = static_cast<std::uint32_t>(shared_->options.block_size);
    auto header = make_header(cipher->key_version(), block_size, bytes.size());
    if (!header) {
        co_return std::unexpected(header.error());
    }
    std::string object = *header;
    for (std::uint64_t index = 0; index * block_size < bytes.size(); ++index) {
        const std::uint64_t length = std::min<std::uint64_t>(block_size, bytes.size() - index * block_size);
        auto sealed = cipher->seal(
            block_associated(shared_->options.workspace_id, key, *header, 'b', index, length),
            std::span<const std::byte>(bytes.data() + index * block_size, length));
        if (!sealed) {
            co_return std::unexpected(sealed.error());
        }
        object += *sealed;
    }
    const std::uint64_t size = object.size();
    // Liquid reads back what it just put (an index run is loaded right after it is written):
    // keep a copy for the window cache when it fits comfortably.
    std::string kept;
    if (size <= shared_->options.cache_bytes / 4) {
        kept = object;
    }
    auto written = co_await shared_->client->put(key, std::move(object), fence_, false);
    if (!written) {
        co_return std::unexpected(written.error());
    }
    auto parsed = parse_header(*header, key);
    shared_->forget(key);
    if (!kept.empty()) {
        shared_->extend(key, parsed->salt_hex, 0, kept);
    }
    std::lock_guard lock(mutex_);
    auto& entry = directory_[key];
    if (entry) {
        for (const auto& [tail, ignored] : entry->tails) {
            stale_tails_.insert(tail);
        }
    }
    entry = std::make_unique<Entry>();
    entry->remote = true;
    entry->ct_size = size;
    apply_header(*entry, std::move(*header), *parsed);
    entry->open_loaded = true;
    dirty_ = true;
    co_return liquid::result<void>{};
}

liquid::runtime::task<void> ZettaObjectBackend::put_if_absent_impl(std::string key,
                                                                   std::vector<std::byte> bytes) {
    if (auto checked = check_liquid_key(key); !checked) {
        co_return std::unexpected(checked.error());
    }
    if (auto ready = co_await ensure_directory(); !ready) {
        co_return std::unexpected(ready.error());
    }
    if (auto ready = co_await ensure_cipher(); !ready) {
        co_return std::unexpected(ready.error());
    }
    {
        std::lock_guard lock(mutex_);
        if (directory_.contains(key)) {
            co_return std::unexpected(
                failure(liquid::errc::already_exists, "the object already exists", key));
        }
    }
    std::shared_ptr<const WorkspaceStoreCipher> cipher;
    {
        std::lock_guard lock(shared_->cipher_mutex);
        cipher = shared_->cipher;
    }
    const auto block_size = static_cast<std::uint32_t>(shared_->options.block_size);
    auto header = make_header(cipher->key_version(), block_size, bytes.size());
    if (!header) {
        co_return std::unexpected(header.error());
    }
    std::string object = *header;
    for (std::uint64_t index = 0; index * block_size < bytes.size(); ++index) {
        const std::uint64_t length = std::min<std::uint64_t>(block_size, bytes.size() - index * block_size);
        auto sealed = cipher->seal(
            block_associated(shared_->options.workspace_id, key, *header, 'b', index, length),
            std::span<const std::byte>(bytes.data() + index * block_size, length));
        if (!sealed) {
            co_return std::unexpected(sealed.error());
        }
        object += *sealed;
    }
    const std::uint64_t size = object.size();
    auto written = co_await shared_->client->put(key, std::move(object), fence_, true);
    if (!written) {
        if (written.error().code == liquid::errc::already_exists) {
            // The directory said the key was absent: another writer exists, which the fence should
            // have excluded, so nothing this backend knows can be trusted. The directory and any
            // buffered appends are kept as they are; the store must be closed and reopened.
            shared_->failed.store(true);
        }
        co_return std::unexpected(written.error());
    }
    auto parsed = parse_header(*header, key);
    std::lock_guard lock(mutex_);
    auto& entry = directory_[key];
    entry = std::make_unique<Entry>();
    entry->remote = true;
    entry->ct_size = size;
    apply_header(*entry, std::move(*header), *parsed);
    entry->open_loaded = true;
    dirty_ = true;
    co_return liquid::result<void>{};
}

liquid::runtime::task<void>
ZettaObjectBackend::put_range_impl(std::string key, std::uint64_t offset, std::vector<std::byte> bytes) {
    if (auto checked = check_liquid_key(key); !checked) {
        co_return std::unexpected(checked.error());
    }
    if (auto ready = co_await ensure_directory(); !ready) {
        co_return std::unexpected(ready.error());
    }
    if (auto ready = co_await ensure_cipher(); !ready) {
        co_return std::unexpected(ready.error());
    }
    {
        std::lock_guard lock(mutex_);
        if (!directory_.contains(key)) {
            if (offset != 0) {
                co_return std::unexpected(failure(
                    liquid::errc::conflict, "an append to a missing object must start at 0", key));
            }
            std::shared_ptr<const WorkspaceStoreCipher> cipher;
            {
                std::lock_guard cipher_lock(shared_->cipher_mutex);
                cipher = shared_->cipher;
            }
            auto header = make_header(cipher->key_version(),
                                      static_cast<std::uint32_t>(shared_->options.block_size), 0);
            if (!header) {
                co_return std::unexpected(header.error());
            }
            auto parsed = parse_header(*header, key);
            auto entry = std::make_unique<Entry>();
            apply_header(*entry, std::move(*header), *parsed);
            entry->open_loaded = true;
            directory_.emplace(key, std::move(entry));
        }
    }
    if (auto ready = co_await ensure_header(key); !ready) {
        co_return std::unexpected(ready.error());
    }
    if (auto ready = co_await ensure_open_region(key); !ready) {
        co_return std::unexpected(ready.error());
    }
    std::uint64_t size = 0;
    {
        std::lock_guard lock(mutex_);
        size = directory_.at(key)->size(key);
    }
    if (offset > size) {
        co_return std::unexpected(failure(
            liquid::errc::conflict,
            std::format("an append at {} is past the object's end at {}", offset, size), key));
    }
    const std::uint64_t overlap = std::min<std::uint64_t>(size - offset, bytes.size());
    if (overlap > 0) {
        auto existing = co_await read_plain(key, offset, overlap);
        if (!existing) {
            co_return std::unexpected(existing.error());
        }
        if (!std::equal(existing->begin(), existing->end(), bytes.begin())) {
            co_return std::unexpected(failure(
                liquid::errc::conflict,
                std::format("the workspace store on Zetta refuses to rewrite {} bytes of {} at "
                            "offset {}: Zetta objects only grow, so liquid's index merge, which "
                            "rewrites a finished run's header, needs liquid#332 (write-once index "
                            "runs) before this store can merge index runs",
                            overlap, key, offset),
                key));
        }
    }
    bool flush = false;
    {
        std::lock_guard lock(mutex_);
        Entry& entry = *directory_.at(key);
        entry.open.insert(entry.open.end(), bytes.begin() + static_cast<std::ptrdiff_t>(overlap),
                          bytes.end());
        flush = (entry.open.size() / entry.block_size) * entry.block_size >= shared_->options.flush_bytes;
    }
    if (flush) {
        co_return co_await flush_entry(key, false);
    }
    co_return liquid::result<void>{};
}

liquid::runtime::task<std::vector<std::byte>>
ZettaObjectBackend::get_range(std::string key, std::uint64_t offset, std::uint64_t size) {
    if (auto checked = check_liquid_key(key); !checked) {
        co_return std::unexpected(checked.error());
    }
    if (auto ready = co_await ensure_directory(); !ready) {
        co_return std::unexpected(ready.error());
    }
    {
        std::lock_guard lock(mutex_);
        if (!directory_.contains(key)) {
            co_return std::unexpected(failure(liquid::errc::not_found, "no such object", key));
        }
    }
    if (auto ready = co_await ensure_cipher(); !ready) {
        co_return std::unexpected(ready.error());
    }
    if (auto ready = co_await ensure_header(key); !ready) {
        co_return std::unexpected(ready.error());
    }
    std::uint64_t object_size = 0;
    {
        std::lock_guard lock(mutex_);
        const auto found = directory_.find(key);
        if (found == directory_.end()) {
            co_return std::unexpected(failure(liquid::errc::not_found, "no such object", key));
        }
        object_size = found->second->size(key);
    }
    if (offset > object_size || size > object_size - offset) {
        co_return std::unexpected(failure(
            liquid::errc::corrupt,
            std::format("range {}+{} is outside the object of {} bytes", offset, size, object_size),
            key));
    }
    if (size == 0) {
        co_return std::vector<std::byte>{};
    }
    co_return co_await read_plain(key, offset, size);
}

liquid::runtime::task<void> ZettaObjectBackend::remove_impl(std::string key) {
    if (auto checked = check_liquid_key(key); !checked) {
        co_return std::unexpected(checked.error());
    }
    if (auto ready = co_await ensure_directory(); !ready) {
        co_return std::unexpected(ready.error());
    }
    bool remote = false;
    std::vector<std::string> tails;
    {
        std::lock_guard lock(mutex_);
        const auto found = directory_.find(key);
        if (found == directory_.end()) {
            co_return std::unexpected(failure(liquid::errc::not_found, "no such object", key));
        }
        remote = found->second->remote;
        for (const auto& [tail, ignored] : found->second->tails) {
            tails.push_back(tail);
        }
    }
    if (remote) {
        auto removed = co_await shared_->client->remove(key, fence_);
        if (!removed && removed.error().code != liquid::errc::not_found) {
            co_return std::unexpected(removed.error());
        }
    }
    for (const std::string& tail : tails) {
        auto removed = co_await shared_->client->remove(tail, fence_);
        if (!removed && removed.error().code != liquid::errc::not_found) {
            co_return std::unexpected(removed.error());
        }
    }
    shared_->forget(key);
    std::lock_guard lock(mutex_);
    directory_.erase(key);
    dirty_ = dirty_ || remote;
    co_return liquid::result<void>{};
}

liquid::runtime::task<std::vector<liquid::store_backend_entry>>
ZettaObjectBackend::list(std::string prefix) {
    if (auto ready = co_await ensure_directory(); !ready) {
        co_return std::unexpected(ready.error());
    }
    std::vector<std::string> keys;
    {
        std::lock_guard lock(mutex_);
        for (auto it = directory_.lower_bound(prefix);
             it != directory_.end() && it->first.starts_with(prefix); ++it) {
            keys.push_back(it->first);
        }
    }
    for (const std::string& key : keys) {
        if (auto ready = co_await ensure_header(key); !ready) {
            co_return std::unexpected(ready.error());
        }
    }
    std::vector<liquid::store_backend_entry> output;
    std::lock_guard lock(mutex_);
    for (const std::string& key : keys) {
        const auto found = directory_.find(key);
        if (found != directory_.end()) {
            output.push_back({key, found->second->size(key)});
        }
    }
    co_return output;
}

liquid::runtime::task<void> ZettaObjectBackend::barrier_impl() {
    std::vector<std::string> pending;
    {
        std::lock_guard lock(mutex_);
        if (!directory_loaded_) {
            co_return liquid::result<void>{};
        }
        for (const auto& [key, entry] : directory_) {
            if (entry->open_loaded && (!entry->remote || entry->open.size() != entry->open_remote)) {
                pending.push_back(key);
            }
        }
    }
    for (const std::string& key : pending) {
        if (auto flushed = co_await flush_entry(key, true); !flushed) {
            co_return std::unexpected(flushed.error());
        }
    }
    bool dirty = false;
    {
        std::lock_guard lock(mutex_);
        dirty = dirty_;
    }
    if (dirty) {
        if (auto synchronized = co_await shared_->client->barrier(); !synchronized) {
            co_return std::unexpected(synchronized.error());
        }
        std::lock_guard lock(mutex_);
        dirty_ = false;
    }
    co_return co_await remove_stale_tails();
}

// A failed mutation can leave liquid's memory of the store ahead of the bucket (an index flush
// that cleared its memtable before the barrier failed, a pack append liquid will retry at the
// same offset). Liquid recovers such a store on its next open, so the backend says it failed and
// lake closes and reopens the store before its next use.
liquid::result<void> ZettaObjectBackend::observe(liquid::result<void> outcome, bool tolerated) {
    if (!outcome && !tolerated) {
        shared_->failed.store(true);
    }
    return outcome;
}

bool ZettaObjectBackend::failed() const {
    return shared_->failed.load();
}

liquid::runtime::task<void> ZettaObjectBackend::put(std::string key, std::vector<std::byte> bytes) {
    co_return observe(co_await put_impl(std::move(key), std::move(bytes)), false);
}

liquid::runtime::task<void> ZettaObjectBackend::put_if_absent(std::string key,
                                                              std::vector<std::byte> bytes) {
    auto outcome = co_await put_if_absent_impl(std::move(key), std::move(bytes));
    const bool tolerated = !outcome && outcome.error().code == liquid::errc::already_exists;
    co_return observe(std::move(outcome), tolerated);
}

liquid::runtime::task<void>
ZettaObjectBackend::put_range(std::string key, std::uint64_t offset, std::vector<std::byte> bytes) {
    co_return observe(co_await put_range_impl(std::move(key), offset, std::move(bytes)), false);
}

liquid::runtime::task<void> ZettaObjectBackend::remove(std::string key) {
    auto outcome = co_await remove_impl(std::move(key));
    const bool tolerated = !outcome && outcome.error().code == liquid::errc::not_found;
    co_return observe(std::move(outcome), tolerated);
}

liquid::runtime::task<void> ZettaObjectBackend::barrier() {
    co_return observe(co_await barrier_impl(), false);
}

// ---- ZettaStoreOwner -------------------------------------------------------------------------

ZettaStoreOwner::ZettaStoreOwner(std::shared_ptr<ZettaObjectClient> client,
                                 std::chrono::milliseconds ttl,
                                 std::function<void()> on_lost)
    : client_(std::move(client)), ttl_(ttl), on_lost_(std::move(on_lost)) {}

ZettaStoreOwner::~ZettaStoreOwner() {
    stop_thread();
}

bool ZettaStoreOwner::held() const {
    std::lock_guard lock(mutex_);
    return held_;
}

bool ZettaStoreOwner::lost() const {
    if (lost_.load()) {
        return true;
    }
    // The store's copy of the grant shares its state: liquid revokes it when Zetta refuses a write
    // as fenced, before this owner's next renewal finds out.
    std::lock_guard lock(mutex_);
    return held_ && grant_.valid() && grant_.lost();
}

std::uint64_t ZettaStoreOwner::fence() const {
    std::lock_guard lock(mutex_);
    return grant_.valid() ? grant_.fence() : 0;
}

void ZettaStoreOwner::stop_thread() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (renewer_.joinable() && renewer_.get_id() != std::this_thread::get_id()) {
        renewer_.join();
    }
}

void ZettaStoreOwner::stop_renewing() {
    stop_thread();
}

liquid::runtime::task<liquid::owner_grant>
ZettaStoreOwner::acquire(std::string holder, std::chrono::milliseconds wait) {
    stop_thread();
    // Zetta lets a holder re-acquire a lease it holds. Two opens in one process describe
    // themselves alike, so each owner adds its own token: a second open waits like any other.
    {
        std::array<unsigned char, 8> token{};
        if (RAND_bytes(token.data(), static_cast<int>(token.size())) != 1) {
            co_return std::unexpected(failure(liquid::errc::internal, "could not name the lease holder"));
        }
        holder += " (lease " + hex_of(token) + ")";
    }
    const auto deadline = std::chrono::steady_clock::now() + wait;
    for (;;) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        const auto piece = std::clamp(remaining, std::chrono::milliseconds(0),
                                      std::chrono::milliseconds(25'000));
        auto lease = co_await client_->acquire(holder, ttl_, piece);
        if (lease) {
            liquid::owner_grant grant(lease->fence);
            {
                std::lock_guard lock(mutex_);
                holder_ = holder;
                grant_ = grant;
                held_ = true;
                stopping_ = false;
            }
            lost_.store(false);
            renewer_ = std::thread([this] { renew_loop(); });
            co_return grant;
        }
        if (lease.error().code != liquid::errc::locked ||
            std::chrono::steady_clock::now() >= deadline) {
            co_return std::unexpected(lease.error());
        }
    }
}

void ZettaStoreOwner::renew_loop() {
    auto expires = std::chrono::steady_clock::now() + ttl_;
    std::unique_lock lock(mutex_);
    while (!stopping_) {
        wake_.wait_for(lock, ttl_ / 3, [this] { return stopping_; });
        if (stopping_) {
            return;
        }
        const std::string holder = holder_;
        const std::uint64_t fence = grant_.fence();
        lock.unlock();
        // The lease runs from when Zetta could have granted it at the earliest: the send.
        const auto sent = std::chrono::steady_clock::now();
        auto renewed = liquid::runtime::block_on(client_->renew(holder, fence, ttl_, ttl_ / 3));
        lock.lock();
        if (stopping_) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (renewed) {
            expires = sent + ttl_;
            continue;
        }
        // Refused as fenced, or no renewal accepted before the lease ran out: another holder may
        // write now, so this one must not.
        if (renewed.error().code == liquid::errc::locked || now + ttl_ / 6 >= expires) {
            grant_.revoke();
            lost_.store(true);
            std::function<void()> notify = on_lost_;
            lock.unlock();
            if (notify) {
                notify();
            }
            return;
        }
    }
}

liquid::runtime::task<void> ZettaStoreOwner::release() {
    stop_thread();
    std::string holder;
    std::uint64_t fence = 0;
    bool release = false;
    {
        std::lock_guard lock(mutex_);
        release = held_ && !lost_.load();
        holder = holder_;
        fence = grant_.valid() ? grant_.fence() : 0;
        held_ = false;
        if (grant_.valid()) {
            grant_.revoke();
        }
    }
    if (release) {
        auto released = co_await client_->release(holder, fence);
        if (!released && released.error().code != liquid::errc::locked) {
            co_return std::unexpected(released.error());
        }
    }
    co_return liquid::result<void>{};
}

} // namespace lake::control
