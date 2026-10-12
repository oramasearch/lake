// A customer's own bucket browsed as a drive (docs/api/storage.md#browsing-a-bucket, lake#664):
// one BucketContents over a small object protocol, with two implementations written here — S3
// (SigV4: Cloudflare R2, AWS S3, Backblaze B2, Google Cloud Storage's XML API) and Azure Blob
// (Shared Key or a SAS). No provider SDK: each request is built, signed and parsed here, and sent
// through the injected StoreHttp.

#include "lake/control/store_buckets.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <ctime>
#include <format>
#include <set>

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "lake/control/agent_bus.hpp"
#include "lake/control/drives.hpp"
#include "lake/control/store.hpp"

namespace lake::control {
namespace {

template <class T> using Result = ZettaControlResult<T>;

template <class T> Result<T> refused(std::string code, std::string message) {
    return std::unexpected(ZettaControlError{std::move(code), std::move(message)});
}

// A refusal any Result<T> coroutine can co_return (a bare std::unexpected would convert to the
// task's own liquid::result).
struct Failed {
    ZettaControlError error;
    template <class T> operator Result<T>() const {
        return std::unexpected(error);
    }
};
Failed failed(ZettaControlError error) {
    return {std::move(error)};
}

ZettaControlError transport_error(const liquid::error& error) {
    return {"store_unavailable", "the bucket did not answer: " + error.message};
}

std::string lower(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

std::string trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.remove_suffix(1);
    }
    return std::string(text);
}

std::string hex(const unsigned char* data, std::size_t size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (std::size_t index = 0; index < size; ++index) {
        out.push_back(digits[data[index] >> 4]);
        out.push_back(digits[data[index] & 0x0f]);
    }
    return out;
}

std::string sha256_hex(std::string_view bytes) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    EVP_Digest(bytes.data(), bytes.size(), digest, &size, EVP_sha256(), nullptr);
    return hex(digest, size);
}

std::string hmac_sha256(std::string_view key, std::string_view message) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    HMAC(EVP_sha256(),
         key.data(),
         static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(message.data()),
         message.size(),
         digest,
         &size);
    return std::string(reinterpret_cast<const char*>(digest), size);
}

std::string base64_encode(std::string_view bytes) {
    std::string out(4 * ((bytes.size() + 2) / 3), '\0');
    const int size = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                     reinterpret_cast<const unsigned char*>(bytes.data()),
                                     static_cast<int>(bytes.size()));
    out.resize(static_cast<std::size_t>(std::max(size, 0)));
    return out;
}

std::optional<std::string> base64_decode(std::string_view text) {
    if (text.empty() || text.size() % 4 != 0) {
        return std::nullopt;
    }
    std::string out((text.size() / 4) * 3, '\0');
    const int size = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                     reinterpret_cast<const unsigned char*>(text.data()),
                                     static_cast<int>(text.size()));
    if (size < 0) {
        return std::nullopt;
    }
    std::size_t padding = 0;
    if (text.back() == '=') {
        padding = text[text.size() - 2] == '=' ? 2 : 1;
    }
    out.resize(static_cast<std::size_t>(size) - padding);
    return out;
}

std::tm utc(std::chrono::system_clock::time_point at) {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(at);
    std::tm out{};
    gmtime_r(&seconds, &out);
    return out;
}

std::string format_time(std::chrono::system_clock::time_point at, const char* pattern) {
    const std::tm parts = utc(at);
    char buffer[64];
    const std::size_t size = std::strftime(buffer, sizeof(buffer), pattern, &parts);
    return std::string(buffer, size);
}

// "2026-10-11T12:00:00.000Z" (S3) to "2026-10-11T12:00:00Z".
std::string rfc3339_from_iso(std::string_view text) {
    if (text.size() < 19) {
        return std::string(text);
    }
    return std::string(text.substr(0, 19)) + "Z";
}

// "Sun, 11 Oct 2026 12:00:00 GMT" (Azure) to "2026-10-11T12:00:00Z".
std::string rfc3339_from_http_date(std::string_view text) {
    static constexpr std::array<std::string_view, 12> months{
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    // "Www, DD Mon YYYY HH:MM:SS GMT"
    if (text.size() < 29 || text[3] != ',') {
        return std::string(text);
    }
    const std::string_view month = text.substr(8, 3);
    const auto found = std::ranges::find(months, month);
    if (found == months.end()) {
        return std::string(text);
    }
    return std::format("{}-{:02}-{}T{}Z",
                       text.substr(12, 4),
                       static_cast<int>(found - months.begin()) + 1,
                       text.substr(5, 2),
                       text.substr(17, 8));
}

// ---- A small XML reader: enough for the list answers of S3 and Azure ----

std::string xml_unescape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] != '&') {
            out.push_back(text[index]);
            continue;
        }
        const std::size_t end = text.find(';', index);
        if (end == std::string_view::npos) {
            out.push_back('&');
            continue;
        }
        const std::string_view entity = text.substr(index + 1, end - index - 1);
        if (entity == "amp") {
            out.push_back('&');
        } else if (entity == "lt") {
            out.push_back('<');
        } else if (entity == "gt") {
            out.push_back('>');
        } else if (entity == "quot") {
            out.push_back('"');
        } else if (entity == "apos") {
            out.push_back('\'');
        } else if (entity.starts_with("#")) {
            unsigned long code = 0;
            try {
                code = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X')
                           ? std::stoul(std::string(entity.substr(2)), nullptr, 16)
                           : std::stoul(std::string(entity.substr(1)));
            } catch (...) {
                out.append(text.substr(index, end - index + 1));
                index = end;
                continue;
            }
            // UTF-8 encode.
            if (code < 0x80) {
                out.push_back(static_cast<char>(code));
            } else if (code < 0x800) {
                out.push_back(static_cast<char>(0xc0 | (code >> 6)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
            } else if (code < 0x10000) {
                out.push_back(static_cast<char>(0xe0 | (code >> 12)));
                out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
            } else {
                out.push_back(static_cast<char>(0xf0 | (code >> 18)));
                out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
                out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
            }
        } else {
            out.append(text.substr(index, end - index + 1));
        }
        index = end;
    }
    return out;
}

// The raw contents of every <tag>…</tag> in `xml`, in order (no nesting of the same tag).
std::vector<std::string_view> xml_blocks(std::string_view xml, std::string_view tag) {
    std::vector<std::string_view> out;
    const std::string open = "<" + std::string(tag) + ">";
    const std::string close = "</" + std::string(tag) + ">";
    std::size_t at = 0;
    while ((at = xml.find(open, at)) != std::string_view::npos) {
        const std::size_t start = at + open.size();
        const std::size_t end = xml.find(close, start);
        if (end == std::string_view::npos) {
            break;
        }
        out.push_back(xml.substr(start, end - start));
        at = end + close.size();
    }
    return out;
}

// The text of the first <tag>, unescaped; empty for none or <tag/>.
std::string xml_text(std::string_view xml, std::string_view tag) {
    const auto blocks = xml_blocks(xml, tag);
    return blocks.empty() ? std::string() : xml_unescape(blocks.front());
}

std::uint64_t to_size(std::string_view text) {
    std::uint64_t out = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return out;
        }
        out = out * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return out;
}

// %XX (and '+' as a space when `plus`) decoded.
std::string percent_decode(std::string_view text, bool plus) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char c = text[index];
        if (c == '+' && plus) {
            out.push_back(' ');
        } else if (c == '%' && index + 2 < text.size() &&
                   std::isxdigit(static_cast<unsigned char>(text[index + 1])) != 0 &&
                   std::isxdigit(static_cast<unsigned char>(text[index + 2])) != 0) {
            out.push_back(static_cast<char>(
                std::stoi(std::string(text.substr(index + 1, 2)), nullptr, 16)));
            index += 2;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

struct Url {
    std::string scheme, host, port, path;
};

std::optional<Url> parse_url(std::string_view url) {
    Url out;
    if (url.starts_with("https://")) {
        out.scheme = "https";
        url.remove_prefix(8);
    } else if (url.starts_with("http://")) {
        out.scheme = "http";
        url.remove_prefix(7);
    } else {
        return std::nullopt;
    }
    const std::size_t slash = url.find('/');
    const std::string_view authority = url.substr(0, slash);
    out.path = slash == std::string_view::npos ? std::string() : std::string(url.substr(slash));
    if (authority.empty() || authority.find('@') != std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t colon = authority.rfind(':');
    if (colon != std::string_view::npos && authority.find(']') == std::string_view::npos) {
        out.host = std::string(authority.substr(0, colon));
        out.port = std::string(authority.substr(colon + 1));
        if (out.port.empty() || !std::ranges::all_of(out.port, [](unsigned char c) {
                return std::isdigit(c) != 0;
            })) {
            return std::nullopt;
        }
    } else {
        out.host = std::string(authority);
    }
    if (out.host.empty()) {
        return std::nullopt;
    }
    return out;
}

// host, or host:port when the port is not the scheme's default: what the Host header carries.
std::string host_header(const Url& url) {
    const bool standard = url.port.empty() || (url.scheme == "https" && url.port == "443") ||
                          (url.scheme == "http" && url.port == "80");
    return standard ? url.host : url.host + ":" + url.port;
}

std::string query_string(std::vector<std::pair<std::string, std::string>> query) {
    std::ranges::sort(query);
    std::string out;
    for (const auto& [name, value] : query) {
        if (!out.empty()) {
            out += '&';
        }
        out += sigv4_encode(name, false) + "=" + sigv4_encode(value, false);
    }
    return out;
}

// ---- The object protocol and the contents over it ----

struct BucketObject {
    std::string key;
    std::uint64_t size = 0;
    std::string etag, modified_at, content_type;
};
struct BucketPage {
    std::vector<BucketObject> objects;
    std::vector<std::string> prefixes;
    std::string next; // empty: the last page
};

class BucketProtocol {
public:
    virtual ~BucketProtocol() = default;
    // Keys under `prefix`, `delimited` by "/" (common prefixes kept as prefixes), at most `max`.
    virtual liquid::runtime::task<Result<BucketPage>>
    list(std::string prefix, bool delimited, std::string next, int max) = 0;
    virtual liquid::runtime::task<Result<std::string>>
    get(std::string key, std::uint64_t offset, std::uint64_t length) = 0;
    // Answers the new object's ETag, when the provider sends one.
    virtual liquid::runtime::task<Result<std::string>>
    put(std::string key, std::string bytes, std::string content_type) = 0;
    virtual liquid::runtime::task<Result<void>> copy(std::string from, std::string to) = 0;
    // Removing a key that is not there is not an error.
    virtual liquid::runtime::task<Result<void>> remove(std::string key) = 0;
};

// The most items one folder lists, and the most objects a folder move or removal touches.
constexpr std::size_t max_folder_items = 10'000;
constexpr std::size_t max_tree_objects = 10'000;

bool item_order(const StoreItem& left, const StoreItem& right) {
    if (left.folder != right.folder) {
        return left.folder;
    }
    return left.path < right.path;
}

// A store's contents over a flat bucket: an item's key is the store's prefix, "/", and its path;
// a folder is every key under "<path>/", made visible while empty by a zero-byte marker object
// "<path>/" (the convention the providers' own consoles use).
class BucketContents final : public StoreContents {
public:
    BucketContents(std::shared_ptr<BucketProtocol> protocol, std::string prefix)
        : protocol_(std::move(protocol)), base_(prefix.empty() ? std::string() : prefix + "/") {}

    liquid::runtime::task<Result<std::vector<StoreItem>>> list(std::string folder) override {
        const std::string under = base_ + (folder.empty() ? std::string() : folder + "/");
        std::vector<StoreItem> items;
        std::set<std::string> seen;
        std::string next;
        do {
            auto page = settled(co_await protocol_->list(under, true, next, 1000));
            if (!page) {
                co_return failed(page.error());
            }
            for (const std::string& prefix : page->prefixes) {
                std::string name = prefix.substr(under.size());
                if (name.ends_with('/')) {
                    name.pop_back();
                }
                if (valid_drive_name(name) && seen.insert(name).second) {
                    items.push_back({join_store_path(folder, name), true});
                }
            }
            for (const BucketObject& object : page->objects) {
                if (object.key.size() <= under.size()) {
                    continue; // the folder's own marker
                }
                const std::string name = object.key.substr(under.size());
                // A file whose name a folder already has is not shown: a drive path is one item.
                if (valid_drive_name(name) && seen.insert(name).second) {
                    items.push_back(item(join_store_path(folder, name), object));
                }
            }
            next = page->next;
        } while (!next.empty() && items.size() < max_folder_items);
        std::ranges::sort(items, item_order);
        co_return items;
    }

    liquid::runtime::task<Result<std::optional<StoreItem>>> stat(std::string path) override {
        if (path.empty()) {
            co_return std::optional<StoreItem>(StoreItem{"", true});
        }
        // The key itself sorts first among the keys it prefixes.
        auto file = settled(co_await first(base_ + path));
        if (!file) {
            co_return failed(file.error());
        }
        if (*file && (*file)->key == base_ + path) {
            co_return std::optional<StoreItem>(item(path, **file));
        }
        auto inside = settled(co_await any_under(base_ + path + "/"));
        if (!inside) {
            co_return failed(inside.error());
        }
        if (*inside) {
            co_return std::optional<StoreItem>(StoreItem{path, true});
        }
        co_return std::optional<StoreItem>();
    }

    liquid::runtime::task<Result<std::string>>
    read(std::string path, std::uint64_t offset, std::uint64_t length) override {
        if (length == 0) {
            co_return std::string();
        }
        co_return settled(co_await protocol_->get(base_ + path, offset, length));
    }

    liquid::runtime::task<Result<StoreItem>>
    write(std::string path, std::string bytes, std::string content_type) override {
        auto there = settled(co_await stat(path));
        if (!there) {
            co_return failed(there.error());
        }
        if (*there && (*there)->folder) {
            co_return refused<StoreItem>("conflict", "a folder is at " + path);
        }
        auto blocked = settled(co_await file_above(path));
        if (!blocked) {
            co_return failed(blocked.error());
        }
        if (*blocked) {
            co_return refused<StoreItem>("conflict", "a file is at " + **blocked);
        }
        const std::uint64_t size = bytes.size();
        auto put = settled(co_await protocol_->put(base_ + path, std::move(bytes), content_type));
        if (!put) {
            co_return failed(put.error());
        }
        StoreItem out{path, false, size, content_type, now_rfc3339(), *put};
        co_return out;
    }

    liquid::runtime::task<Result<void>> make_folder(std::string path) override {
        if (path.empty()) {
            co_return Result<void>();
        }
        auto there = settled(co_await stat(path));
        if (!there) {
            co_return failed(there.error());
        }
        if (*there) {
            if ((*there)->folder) {
                co_return Result<void>();
            }
            co_return refused<void>("conflict", "a file is at " + path);
        }
        auto blocked = settled(co_await file_above(path));
        if (!blocked) {
            co_return failed(blocked.error());
        }
        if (*blocked) {
            co_return refused<void>("conflict", "a file is at " + **blocked);
        }
        auto put = settled(co_await protocol_->put(base_ + path + "/", std::string(), ""));
        if (!put) {
            co_return failed(put.error());
        }
        co_return Result<void>();
    }

    liquid::runtime::task<Result<void>> move(std::string from, std::string to) override {
        auto source = settled(co_await stat(from));
        if (!source) {
            co_return failed(source.error());
        }
        if (!*source) {
            co_return refused<void>("not_found", "nothing is at " + from);
        }
        auto target = settled(co_await stat(to));
        if (!target) {
            co_return failed(target.error());
        }
        if (*target) {
            co_return refused<void>("conflict", "something is already at " + to);
        }
        if (!(*source)->folder) {
            co_return settled(co_await move_key(base_ + from, base_ + to));
        }
        auto keys = settled(co_await tree(base_ + from + "/"));
        if (!keys) {
            co_return failed(keys.error());
        }
        // Copy everything first, then delete: an interrupted move leaves a copy, never a loss.
        for (const std::string& key : *keys) {
            auto copied = co_await protocol_->copy(key, base_ + to + key.substr((base_ + from).size()));
            if (auto trouble = problem(copied)) {
                co_return failed(*trouble);
            }
        }
        for (const std::string& key : *keys) {
            auto removed = co_await protocol_->remove(key);
            if (auto trouble = problem(removed)) {
                co_return failed(*trouble);
            }
        }
        co_return Result<void>();
    }

    liquid::runtime::task<Result<void>> remove(std::string path) override {
        if (path.empty()) {
            co_return refused<void>("invalid_input", "the store itself is never removed");
        }
        auto there = settled(co_await stat(path));
        if (!there) {
            co_return failed(there.error());
        }
        if (!*there) {
            co_return refused<void>("not_found", "nothing is at " + path);
        }
        if (!(*there)->folder) {
            auto removed = co_await protocol_->remove(base_ + path);
            if (auto trouble = problem(removed)) {
                co_return failed(*trouble);
            }
            co_return Result<void>();
        }
        auto keys = settled(co_await tree(base_ + path + "/"));
        if (!keys) {
            co_return failed(keys.error());
        }
        for (const std::string& key : *keys) {
            auto removed = co_await protocol_->remove(key);
            if (auto trouble = problem(removed)) {
                co_return failed(*trouble);
            }
        }
        co_return Result<void>();
    }

private:
    static std::string now_rfc3339() {
        return format_time(std::chrono::system_clock::now(), "%Y-%m-%dT%H:%M:%SZ");
    }

    static StoreItem item(std::string path, const BucketObject& object) {
        return {std::move(path), false, object.size, object.content_type, object.modified_at,
                object.etag};
    }

    template <class T>
    static std::optional<ZettaControlError> problem(const liquid::result<Result<T>>& done) {
        if (!done) {
            return transport_error(done.error());
        }
        if (!*done) {
            return done->error();
        }
        return std::nullopt;
    }

    // The first object whose key starts with `prefix`, undelimited.
    liquid::runtime::task<Result<std::optional<BucketObject>>> first(std::string prefix) {
        auto page = co_await protocol_->list(prefix, false, "", 1);
        if (auto trouble = problem(page)) {
            co_return failed(*trouble);
        }
        if ((*page)->objects.empty()) {
            co_return std::optional<BucketObject>();
        }
        co_return std::optional<BucketObject>((*page)->objects.front());
    }

    liquid::runtime::task<Result<bool>> any_under(std::string prefix) {
        auto found = settled(co_await first(prefix));
        if (!found) {
            co_return failed(found.error());
        }
        co_return found->has_value();
    }

    // The nearest level above `path` that is a file, if any.
    liquid::runtime::task<Result<std::optional<std::string>>> file_above(std::string path) {
        for (std::string level = store_parent(path); !level.empty(); level = store_parent(level)) {
            auto found = settled(co_await first(base_ + level));
            if (!found) {
                co_return failed(found.error());
            }
            if (*found && (*found)->key == base_ + level) {
                co_return std::optional<std::string>(level);
            }
        }
        co_return std::optional<std::string>();
    }

    // Every key under `prefix`, the folder's marker included.
    liquid::runtime::task<Result<std::vector<std::string>>> tree(std::string prefix) {
        std::vector<std::string> keys;
        std::string next;
        do {
            auto page = co_await protocol_->list(prefix, false, next, 1000);
            if (auto trouble = problem(page)) {
                co_return failed(*trouble);
            }
            for (const BucketObject& object : (*page)->objects) {
                keys.push_back(object.key);
            }
            next = (*page)->next;
            if (keys.size() > max_tree_objects) {
                co_return refused<std::vector<std::string>>(
                    "invalid_input",
                    std::format("a folder of more than {} objects is moved or removed with the "
                                "provider's own tools",
                                max_tree_objects));
            }
        } while (!next.empty());
        co_return keys;
    }

    liquid::runtime::task<Result<void>> move_key(std::string from, std::string to) {
        auto copied = co_await protocol_->copy(from, to);
        if (auto trouble = problem(copied)) {
            co_return failed(*trouble);
        }
        auto removed = co_await protocol_->remove(from);
        if (auto trouble = problem(removed)) {
            co_return failed(*trouble);
        }
        co_return Result<void>();
    }

    std::shared_ptr<BucketProtocol> protocol_;
    std::string base_;
};

// The API's error for a provider's answer: 404 is not_found, 409/412 conflict, anything else the
// provider refusing (store_unavailable, with its code and message).
ZettaControlError answer_error(const StoreHttpResponse& response, std::string_view what) {
    if (!response.error.empty()) {
        return {"store_unavailable", "the bucket did not answer: " + response.error};
    }
    std::string code = xml_text(response.body, "Code");
    std::string message = xml_text(response.body, "Message");
    std::string detail = std::to_string(response.status);
    if (!code.empty()) {
        detail += " " + code;
    }
    if (!message.empty()) {
        detail += ": " + message;
    }
    if (response.status == 404 && code != "NoSuchBucket" && code != "ContainerNotFound") {
        return {"not_found", std::string(what) + " is not in the bucket (" + detail + ")"};
    }
    if (response.status == 409 || response.status == 412) {
        return {"conflict", std::string(what) + ": the bucket answered " + detail};
    }
    if (response.status == 401 || response.status == 403) {
        return {"store_unavailable",
                "the bucket refused the workspace's credential (" + detail +
                    "); check the secret this storage names"};
    }
    return {"store_unavailable", "the bucket answered " + detail};
}

// ---- S3 ----

class S3Protocol final : public BucketProtocol {
public:
    S3Protocol(S3Bucket bucket, StoreHttp http, StoreClock clock)
        : bucket_(std::move(bucket)), http_(std::move(http)), clock_(std::move(clock)) {
        url_ = *parse_url(bucket_.endpoint);
        if (bucket_.virtual_host) {
            url_.host = bucket_.bucket + "." + url_.host;
            root_ = "/";
        } else {
            root_ = "/" + sigv4_encode(bucket_.bucket, false) + "/";
        }
    }

    liquid::runtime::task<Result<BucketPage>>
    list(std::string prefix, bool delimited, std::string next, int max) override {
        std::vector<std::pair<std::string, std::string>> query{
            {"list-type", "2"},
            {"prefix", prefix},
            {"max-keys", std::to_string(max)},
            {"encoding-type", "url"}};
        if (delimited) {
            query.emplace_back("delimiter", "/");
        }
        if (!next.empty()) {
            query.emplace_back("continuation-token", next);
        }
        // The bucket itself: "/<bucket>" path-style, "/" on its own host.
        const std::string path = bucket_.virtual_host ? "/" : root_.substr(0, root_.size() - 1);
        auto answered = settled(co_await send("GET", path, std::move(query), {}, std::string()));
        if (!answered) {
            co_return failed(answered.error());
        }
        const StoreHttpResponse& response = *answered;
        if (response.status != 200) {
            co_return failed(answer_error(response, "the folder"));
        }
        const bool encoded = xml_text(response.body, "EncodingType") == "url";
        const auto key = [&](std::string text) {
            return encoded ? percent_decode(text, true) : text;
        };
        BucketPage page;
        for (const std::string_view contents : xml_blocks(response.body, "Contents")) {
            BucketObject object;
            object.key = key(xml_text(contents, "Key"));
            object.size = to_size(xml_text(contents, "Size"));
            object.etag = xml_text(contents, "ETag");
            object.modified_at = rfc3339_from_iso(xml_text(contents, "LastModified"));
            page.objects.push_back(std::move(object));
        }
        for (const std::string_view common : xml_blocks(response.body, "CommonPrefixes")) {
            page.prefixes.push_back(key(xml_text(common, "Prefix")));
        }
        if (xml_text(response.body, "IsTruncated") == "true") {
            page.next = xml_text(response.body, "NextContinuationToken");
        }
        co_return page;
    }

    liquid::runtime::task<Result<std::string>>
    get(std::string key, std::uint64_t offset, std::uint64_t length) override {
        auto answered = settled(co_await send(
            "GET",
            object_path(key),
            {},
            {{"range", std::format("bytes={}-{}", offset, offset + length - 1)}},
            std::string(),
            length + 64 * 1024));
        if (!answered) {
            co_return failed(answered.error());
        }
        StoreHttpResponse& response = *answered;
        if (response.status == 206) {
            co_return std::move(response.body);
        }
        if (response.status == 200) {
            co_return response.body.substr(std::min<std::uint64_t>(offset, response.body.size()),
                                           length);
        }
        if (response.status == 416) {
            co_return refused<std::string>("invalid_input", "the range is past the file's end");
        }
        co_return failed(answer_error(response, "the file"));
    }

    liquid::runtime::task<Result<std::string>>
    put(std::string key, std::string bytes, std::string content_type) override {
        std::vector<std::pair<std::string, std::string>> headers;
        if (!content_type.empty()) {
            headers.emplace_back("content-type", content_type);
        }
        auto answered = settled(co_await send("PUT", object_path(key), {}, std::move(headers), std::move(bytes)));
        if (!answered) {
            co_return failed(answered.error());
        }
        const StoreHttpResponse& response = *answered;
        if (response.status != 200) {
            co_return failed(answer_error(response, "the file"));
        }
        co_return response.header("etag");
    }

    liquid::runtime::task<Result<void>> copy(std::string from, std::string to) override {
        auto answered = settled(co_await send(
            "PUT",
            object_path(to),
            {},
            {{"x-amz-copy-source", "/" + sigv4_encode(bucket_.bucket, false) + "/" +
                                       sigv4_encode(from, true)}},
            std::string()));
        if (!answered) {
            co_return failed(answered.error());
        }
        const StoreHttpResponse& response = *answered;
        // A copy can fail after its 200 has been sent: the body then holds an <Error>.
        if (response.status != 200 || response.body.find("<Error>") != std::string::npos) {
            co_return failed(answer_error(response, "the file"));
        }
        co_return Result<void>();
    }

    liquid::runtime::task<Result<void>> remove(std::string key) override {
        auto answered = settled(co_await send("DELETE", object_path(key), {}, {}, std::string()));
        if (!answered) {
            co_return failed(answered.error());
        }
        const StoreHttpResponse& response = *answered;
        if (response.status != 204 && response.status != 200 && response.status != 404) {
            co_return failed(answer_error(response, "the file"));
        }
        co_return Result<void>();
    }

private:
    std::string object_path(const std::string& key) const {
        return root_ + sigv4_encode(key, true);
    }

    liquid::runtime::task<Result<StoreHttpResponse>>
    send(std::string method,
         std::string path,
         std::vector<std::pair<std::string, std::string>> query,
         std::vector<std::pair<std::string, std::string>> headers,
         std::string body,
         std::size_t max_response = 8 * 1024 * 1024) {
        SigV4Request request{method, host_header(url_), path, query, headers, sha256_hex(body)};
        const auto now = clock_ ? clock_() : std::chrono::system_clock::now();
        auto signed_headers = sigv4_sign(request,
                                         {bucket_.access_key_id,
                                          bucket_.secret_access_key,
                                          bucket_.session_token,
                                          bucket_.region.empty() ? "us-east-1" : bucket_.region},
                                         now);
        StoreHttpRequest out;
        out.method = std::move(method);
        const std::string encoded_query = query_string(std::move(query));
        out.url = url_.scheme + "://" + host_header(url_) + path +
                  (encoded_query.empty() ? "" : "?" + encoded_query);
        out.headers = std::move(headers);
        out.headers.insert(out.headers.end(), signed_headers.begin(), signed_headers.end());
        out.body = std::move(body);
        out.max_response_bytes = max_response;
        auto answered = co_await http_(std::move(out));
        if (!answered) {
            co_return refused<StoreHttpResponse>("store_unavailable",
                                                 transport_error(answered.error()).message);
        }
        if (!answered->error.empty()) {
            co_return refused<StoreHttpResponse>("store_unavailable",
                                                 "the bucket did not answer: " + answered->error);
        }
        co_return std::move(*answered);
    }

    S3Bucket bucket_;
    StoreHttp http_;
    StoreClock clock_;
    Url url_;
    std::string root_;
};

// ---- Azure Blob ----

constexpr std::string_view azure_version = "2021-08-06";

class AzureProtocol final : public BucketProtocol {
public:
    AzureProtocol(AzureContainer container, StoreHttp http, StoreClock clock)
        : container_(std::move(container)), http_(std::move(http)), clock_(std::move(clock)) {
        url_ = *parse_url(container_.endpoint);
        if (container_.account.empty()) {
            container_.account = url_.host.substr(0, url_.host.find('.'));
        }
        std::string sas = container_.sas_token;
        if (sas.starts_with('?')) {
            sas.erase(0, 1);
        }
        container_.sas_token = std::move(sas);
    }

    liquid::runtime::task<Result<BucketPage>>
    list(std::string prefix, bool delimited, std::string next, int max) override {
        std::vector<std::pair<std::string, std::string>> query{{"restype", "container"},
                                                               {"comp", "list"},
                                                               {"prefix", prefix},
                                                               {"maxresults", std::to_string(max)}};
        if (delimited) {
            query.emplace_back("delimiter", "/");
        }
        if (!next.empty()) {
            query.emplace_back("marker", next);
        }
        auto answered = settled(co_await send("GET", container_path(), std::move(query), {}, std::string()));
        if (!answered) {
            co_return failed(answered.error());
        }
        const StoreHttpResponse& response = *answered;
        if (response.status != 200) {
            co_return failed(answer_error(response, "the folder"));
        }
        BucketPage page;
        for (const std::string_view blob : xml_blocks(response.body, "Blob")) {
            BucketObject object;
            object.key = xml_text(blob, "Name");
            object.size = to_size(xml_text(blob, "Content-Length"));
            object.etag = xml_text(blob, "Etag");
            object.content_type = xml_text(blob, "Content-Type");
            object.modified_at = rfc3339_from_http_date(xml_text(blob, "Last-Modified"));
            page.objects.push_back(std::move(object));
        }
        for (const std::string_view prefix_block : xml_blocks(response.body, "BlobPrefix")) {
            page.prefixes.push_back(xml_text(prefix_block, "Name"));
        }
        page.next = xml_text(response.body, "NextMarker");
        co_return page;
    }

    liquid::runtime::task<Result<std::string>>
    get(std::string key, std::uint64_t offset, std::uint64_t length) override {
        auto answered = settled(co_await send(
            "GET",
            blob_path(key),
            {},
            {{"x-ms-range", std::format("bytes={}-{}", offset, offset + length - 1)}},
            std::string(),
            length + 64 * 1024));
        if (!answered) {
            co_return failed(answered.error());
        }
        StoreHttpResponse& response = *answered;
        if (response.status == 206) {
            co_return std::move(response.body);
        }
        if (response.status == 200) {
            co_return response.body.substr(std::min<std::uint64_t>(offset, response.body.size()),
                                           length);
        }
        if (response.status == 416) {
            co_return refused<std::string>("invalid_input", "the range is past the file's end");
        }
        co_return failed(answer_error(response, "the file"));
    }

    liquid::runtime::task<Result<std::string>>
    put(std::string key, std::string bytes, std::string content_type) override {
        std::vector<std::pair<std::string, std::string>> headers{{"x-ms-blob-type", "BlockBlob"}};
        if (!content_type.empty()) {
            headers.emplace_back("content-type", content_type);
        }
        auto answered =
            settled(co_await send("PUT", blob_path(key), {}, std::move(headers), std::move(bytes)));
        if (!answered) {
            co_return failed(answered.error());
        }
        const StoreHttpResponse& response = *answered;
        if (response.status != 201) {
            co_return failed(answer_error(response, "the file"));
        }
        co_return response.header("etag");
    }

    liquid::runtime::task<Result<void>> copy(std::string from, std::string to) override {
        std::string source = url_.scheme + "://" + host_header(url_) + blob_path(from);
        if (!container_.sas_token.empty()) {
            source += "?" + container_.sas_token;
        }
        auto answered =
            settled(co_await send("PUT", blob_path(to), {}, {{"x-ms-copy-source", source}}, std::string()));
        if (!answered) {
            co_return failed(answered.error());
        }
        const StoreHttpResponse& response = *answered;
        if (response.status != 202 && response.status != 201) {
            co_return failed(answer_error(response, "the file"));
        }
        // A copy inside one account normally completes before the answer; otherwise wait for it,
        // reading its status from the listing.
        std::string status = response.header("x-ms-copy-status");
        for (int attempt = 0; status == "pending" && attempt < 30; ++attempt) {
            [[maybe_unused]] auto waited = co_await liquid::runtime::delay(std::chrono::milliseconds(250));
            auto listed = settled(co_await send(
                "GET",
                container_path(),
                {{"restype", "container"}, {"comp", "list"}, {"prefix", to}, {"include", "copy"},
                 {"maxresults", "1"}},
                {},
                std::string()));
            if (!listed) {
                co_return failed(listed.error());
            }
            const auto blobs = xml_blocks(listed->body, "Blob");
            status = blobs.empty() ? std::string("failed") : xml_text(blobs.front(), "CopyStatus");
        }
        if (status != "success" && !status.empty()) {
            co_return refused<void>("store_unavailable",
                                    "the container's copy of " + from + " ended " + status);
        }
        co_return Result<void>();
    }

    liquid::runtime::task<Result<void>> remove(std::string key) override {
        auto answered = settled(co_await send("DELETE", blob_path(key), {}, {}, std::string()));
        if (!answered) {
            co_return failed(answered.error());
        }
        const StoreHttpResponse& response = *answered;
        if (response.status != 202 && response.status != 200 && response.status != 404) {
            co_return failed(answer_error(response, "the file"));
        }
        co_return Result<void>();
    }

private:
    std::string container_path() const {
        return "/" + sigv4_encode(container_.container, false);
    }
    std::string blob_path(const std::string& key) const {
        return container_path() + "/" + sigv4_encode(key, true);
    }

    liquid::runtime::task<Result<StoreHttpResponse>>
    send(std::string method,
         std::string path,
         std::vector<std::pair<std::string, std::string>> query,
         std::vector<std::pair<std::string, std::string>> headers,
         std::string body,
         std::size_t max_response = 8 * 1024 * 1024) {
        const auto now = clock_ ? clock_() : std::chrono::system_clock::now();
        headers.emplace_back("x-ms-date", format_time(now, "%a, %d %b %Y %H:%M:%S GMT"));
        headers.emplace_back("x-ms-version", std::string(azure_version));
        std::string encoded_query = query_string(query);
        if (container_.sas_token.empty()) {
            headers.emplace_back("content-length", std::to_string(body.size()));
            const std::string signature = azure_shared_key_signature(
                container_.account, container_.account_key, method, path, query, headers);
            std::erase_if(headers, [](const auto& header) { return header.first == "content-length"; });
            headers.emplace_back("authorization",
                                 "SharedKey " + container_.account + ":" + signature);
        } else {
            encoded_query += (encoded_query.empty() ? "" : "&") + container_.sas_token;
        }
        StoreHttpRequest out;
        out.method = std::move(method);
        out.url = url_.scheme + "://" + host_header(url_) + path +
                  (encoded_query.empty() ? "" : "?" + encoded_query);
        out.headers = std::move(headers);
        out.body = std::move(body);
        out.max_response_bytes = max_response;
        auto answered = co_await http_(std::move(out));
        if (!answered) {
            co_return refused<StoreHttpResponse>("store_unavailable",
                                                 transport_error(answered.error()).message);
        }
        if (!answered->error.empty()) {
            co_return refused<StoreHttpResponse>("store_unavailable",
                                                 "the container did not answer: " + answered->error);
        }
        co_return std::move(*answered);
    }

    AzureContainer container_;
    StoreHttp http_;
    StoreClock clock_;
    Url url_;
};

std::string credential_field(const std::map<std::string, std::string>& credential,
                             std::string_view name) {
    const auto found = credential.find(std::string(name));
    return found == credential.end() ? std::string() : found->second;
}

} // namespace

std::string StoreHttpResponse::header(std::string_view name) const {
    const auto found = headers.find(lower(name));
    return found == headers.end() ? std::string() : found->second;
}

std::string sigv4_encode(std::string_view text, bool keep_slash) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (std::isalnum(byte) != 0 || c == '-' || c == '_' || c == '.' || c == '~' ||
            (c == '/' && keep_slash)) {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(digits[byte >> 4]);
            out.push_back(digits[byte & 0x0f]);
        }
    }
    return out;
}

std::vector<std::pair<std::string, std::string>>
sigv4_sign(const SigV4Request& request, const SigV4Credentials& credentials,
           std::chrono::system_clock::time_point now) {
    const std::string stamp = format_time(now, "%Y%m%dT%H%M%SZ");
    const std::string date = stamp.substr(0, 8);
    std::vector<std::pair<std::string, std::string>> added{
        {"x-amz-content-sha256", request.payload_sha256}, {"x-amz-date", stamp}};
    if (!credentials.session_token.empty()) {
        added.emplace_back("x-amz-security-token", credentials.session_token);
    }
    std::map<std::string, std::string> canonical;
    canonical["host"] = request.host;
    for (const auto& [name, value] : request.headers) {
        canonical[lower(name)] = trim(value);
    }
    for (const auto& [name, value] : added) {
        canonical[name] = value;
    }
    std::string headers, signed_headers;
    for (const auto& [name, value] : canonical) {
        headers += name + ":" + value + "\n";
        signed_headers += (signed_headers.empty() ? "" : ";") + name;
    }
    const std::string canonical_request = request.method + "\n" +
                                          (request.path.empty() ? "/" : request.path) + "\n" +
                                          query_string(request.query) + "\n" + headers + "\n" +
                                          signed_headers + "\n" + request.payload_sha256;
    const std::string scope =
        date + "/" + credentials.region + "/" + credentials.service + "/aws4_request";
    const std::string to_sign =
        "AWS4-HMAC-SHA256\n" + stamp + "\n" + scope + "\n" + sha256_hex(canonical_request);
    std::string key = hmac_sha256("AWS4" + credentials.secret_access_key, date);
    key = hmac_sha256(key, credentials.region);
    key = hmac_sha256(key, credentials.service);
    key = hmac_sha256(key, "aws4_request");
    const std::string signature = hmac_sha256(key, to_sign);
    added.emplace_back("authorization",
                       "AWS4-HMAC-SHA256 Credential=" + credentials.access_key_id + "/" + scope +
                           ", SignedHeaders=" + signed_headers + ", Signature=" +
                           hex(reinterpret_cast<const unsigned char*>(signature.data()),
                               signature.size()));
    return added;
}

std::string azure_shared_key_signature(std::string_view account,
                                       std::string_view account_key,
                                       std::string_view method,
                                       std::string_view path,
                                       const std::vector<std::pair<std::string, std::string>>& query,
                                       const std::vector<std::pair<std::string, std::string>>& headers) {
    std::map<std::string, std::string> named;
    for (const auto& [name, value] : headers) {
        named[lower(name)] = trim(value);
    }
    const auto field = [&](std::string_view name) {
        const auto found = named.find(std::string(name));
        return found == named.end() ? std::string() : found->second;
    };
    std::string length = field("content-length");
    if (length == "0") {
        length.clear();
    }
    std::string to_sign = std::string(method) + "\n" + field("content-encoding") + "\n" +
                          field("content-language") + "\n" + length + "\n" + field("content-md5") +
                          "\n" + field("content-type") + "\n" + field("date") + "\n" +
                          field("if-modified-since") + "\n" + field("if-match") + "\n" +
                          field("if-none-match") + "\n" + field("if-unmodified-since") + "\n" +
                          field("range") + "\n";
    for (const auto& [name, value] : named) {
        if (name.starts_with("x-ms-")) {
            to_sign += name + ":" + value + "\n";
        }
    }
    to_sign += "/" + std::string(account) + std::string(path);
    std::map<std::string, std::vector<std::string>> parameters;
    for (const auto& [name, value] : query) {
        parameters[lower(name)].push_back(value);
    }
    for (auto& [name, values] : parameters) {
        std::ranges::sort(values);
        std::string joined;
        for (const std::string& value : values) {
            joined += (joined.empty() ? "" : ",") + value;
        }
        to_sign += "\n" + name + ":" + joined;
    }
    const auto key = base64_decode(account_key);
    return base64_encode(hmac_sha256(key ? *key : std::string(), to_sign));
}

std::shared_ptr<StoreContents> s3_store_contents(S3Bucket bucket, StoreHttp http, StoreClock clock) {
    std::string prefix = bucket.prefix;
    return std::make_shared<BucketContents>(
        std::make_shared<S3Protocol>(std::move(bucket), std::move(http), std::move(clock)),
        std::move(prefix));
}

std::shared_ptr<StoreContents>
azure_store_contents(AzureContainer container, StoreHttp http, StoreClock clock) {
    std::string prefix = container.prefix;
    return std::make_shared<BucketContents>(
        std::make_shared<AzureProtocol>(std::move(container), std::move(http), std::move(clock)),
        std::move(prefix));
}

std::string default_store_endpoint(std::string_view provider, std::string_view region) {
    if (provider == "s3") {
        return "https://s3." + std::string(region.empty() ? "us-east-1" : region) + ".amazonaws.com";
    }
    if (provider == "gcs") {
        return "https://storage.googleapis.com";
    }
    if (provider == "b2" && !region.empty()) {
        return "https://s3." + std::string(region) + ".backblazeb2.com";
    }
    return {};
}

std::string store_endpoint_problem(std::string_view endpoint) {
    const auto url = parse_url(endpoint);
    if (!url || url->scheme != "https" || !(url->path.empty() || url->path == "/")) {
        return "endpoint must be https://host or https://host:port, with no path";
    }
    if (endpoint.find_first_of("?#") != std::string_view::npos) {
        return "endpoint must have no query";
    }
    return {};
}

std::string store_bucket_problem(std::string_view provider, std::string_view bucket) {
    const bool azure = provider == "azure";
    if (bucket.size() < 3 || bucket.size() > 63 ||
        std::isalnum(static_cast<unsigned char>(bucket.front())) == 0 ||
        std::isalnum(static_cast<unsigned char>(bucket.back())) == 0 ||
        !std::ranges::all_of(bucket, [&](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                   (c == '.' && !azure) || (c == '_' && provider == "gcs");
        })) {
        return azure ? "the container is 3-63 lowercase letters, digits and '-'"
                     : "the bucket is 3-63 lowercase letters, digits, '.' and '-'";
    }
    return {};
}

std::string store_prefix_problem(std::string_view prefix) {
    if (!prefix.empty() && !valid_store_path(prefix)) {
        return "prefix is a folder in the bucket: names joined by \"/\", with no leading or "
               "trailing \"/\"";
    }
    return {};
}

StoreBrowser cloud_store_browser(StoreHttp http, StoreClock clock) {
    return [http = std::move(http), clock = std::move(clock)](const StoreOpening& opening)
               -> ZettaControlResult<std::shared_ptr<StoreContents>> {
        const ManagedStore& store = opening.store;
        if (!store_external_bucket(store.type, store.provider)) {
            return refused<std::shared_ptr<StoreContents>>(
                "not_supported", store_browse_refusal(store.type, store.provider));
        }
        // The connection the operator delivered names the same bucket; the store's own record is
        // what Lake browses, so a stale connection cannot redirect it.
        const std::string endpoint = store.endpoint_url.empty()
                                         ? default_store_endpoint(store.provider, store.region)
                                         : store.endpoint_url;
        if (const std::string problem = store_endpoint_problem(endpoint); !problem.empty()) {
            return refused<std::shared_ptr<StoreContents>>("invalid_input", problem);
        }
        const auto need = [&](std::string_view field) -> std::optional<ZettaControlError> {
            if (credential_field(opening.credential, field).empty()) {
                return ZettaControlError{"invalid_input",
                                         "the secret " + store.credential_name +
                                             " has no text field " + std::string(field)};
            }
            return std::nullopt;
        };
        if (store.provider == "azure") {
            AzureContainer container{endpoint,
                                     "",
                                     store.bucket,
                                     store.prefix,
                                     credential_field(opening.credential, "account_key"),
                                     credential_field(opening.credential, "sas_token")};
            if (container.account_key.empty() && container.sas_token.empty()) {
                return refused<std::shared_ptr<StoreContents>>(
                    "invalid_input",
                    "the secret " + store.credential_name +
                        " has neither an account_key nor a sas_token field");
            }
            if (!container.account_key.empty() && !base64_decode(container.account_key)) {
                return refused<std::shared_ptr<StoreContents>>(
                    "invalid_input", "the secret's account_key is not base64");
            }
            return azure_store_contents(std::move(container), http, clock);
        }
        for (const std::string_view field : {"access_key_id", "secret_access_key"}) {
            if (auto missing = need(field)) {
                return std::unexpected(*missing);
            }
        }
        S3Bucket bucket;
        bucket.endpoint = endpoint;
        bucket.region = store.region.empty()
                            ? (store.provider == "s3" ? std::string("us-east-1") : std::string("auto"))
                            : store.region;
        bucket.bucket = store.bucket;
        bucket.prefix = store.prefix;
        bucket.access_key_id = credential_field(opening.credential, "access_key_id");
        bucket.secret_access_key = credential_field(opening.credential, "secret_access_key");
        bucket.session_token = credential_field(opening.credential, "session_token");
        // AWS S3 addresses a bucket on its own host; the others take it in the path.
        bucket.virtual_host = store.provider == "s3" && endpoint.ends_with(".amazonaws.com") &&
                              store.bucket.find('.') == std::string::npos;
        return s3_store_contents(std::move(bucket), http, clock);
    };
}

} // namespace lake::control
