// SPDX-License-Identifier: MIT

#pragma once

// Local fakes of the providers a store drive browses (lake#664): an S3 server that checks SigV4,
// an Azure Blob server that checks Shared Key or a SAS, and a liquid repository branch in memory.
// Used by test_store_providers.cpp (the conformance suite) and test_drives.cpp (the capabilities).

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <format>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include "api/store_repository.hpp"
#include "lake/control/store_buckets.hpp"

namespace lake::test::stores {

using json = nlohmann::json;
using lake::control::StoreContents;
using lake::control::StoreHttpRequest;
using lake::control::StoreHttpResponse;
using lake::control::StoreItem;

inline std::chrono::system_clock::time_point at(int year, int month, int day, int hour = 0) {
    std::tm parts{};
    parts.tm_year = year - 1900;
    parts.tm_mon = month - 1;
    parts.tm_mday = day;
    parts.tm_hour = hour;
    return std::chrono::system_clock::from_time_t(timegm(&parts));
}

inline std::string sha256_hex(std::string_view bytes) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    EVP_Digest(bytes.data(), bytes.size(), digest, &size, EVP_sha256(), nullptr);
    std::string out;
    for (unsigned index = 0; index < size; ++index) {
        out += std::format("{:02x}", digest[index]);
    }
    return out;
}

inline std::string decode(std::string_view text, bool plus = false) {
    std::string out;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '%' && index + 2 < text.size()) {
            out.push_back(static_cast<char>(std::stoi(std::string(text.substr(index + 1, 2)), nullptr, 16)));
            index += 2;
        } else if (text[index] == '+' && plus) {
            out.push_back(' ');
        } else {
            out.push_back(text[index]);
        }
    }
    return out;
}

inline std::string xml_escape(std::string_view text) {
    std::string out;
    for (const char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        default: out.push_back(c);
        }
    }
    return out;
}

struct Request {
    std::string host, path;
    std::vector<std::pair<std::string, std::string>> query;
    std::string param(std::string_view name) const {
        for (const auto& [key, value] : query) {
            if (key == name) {
                return value;
            }
        }
        return {};
    }
    bool has(std::string_view name) const {
        return std::ranges::any_of(query, [&](const auto& pair) { return pair.first == name; });
    }
};

inline Request parse(const std::string& url) {
    Request out;
    std::string rest = url.substr(url.find("://") + 3);
    const std::size_t slash = rest.find('/');
    out.host = rest.substr(0, slash);
    rest = rest.substr(slash);
    const std::size_t question = rest.find('?');
    out.path = rest.substr(0, question);
    if (question != std::string::npos) {
        std::string query = rest.substr(question + 1);
        std::size_t start = 0;
        while (start <= query.size()) {
            const std::size_t amp = query.find('&', start);
            const std::string pair = query.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
            const std::size_t equals = pair.find('=');
            out.query.emplace_back(decode(pair.substr(0, equals)),
                                   equals == std::string::npos ? "" : decode(pair.substr(equals + 1)));
            if (amp == std::string::npos) {
                break;
            }
            start = amp + 1;
        }
    }
    return out;
}

inline std::string header(const StoreHttpRequest& request, std::string_view name) {
    for (const auto& [key, value] : request.headers) {
        if (key == name) {
            return value;
        }
    }
    return {};
}

inline StoreHttpResponse answer(int status, std::string body = {}, std::map<std::string, std::string> headers = {}) {
    StoreHttpResponse out;
    out.status = status;
    out.body = std::move(body);
    out.headers = std::move(headers);
    return out;
}

// One object of a fake bucket.
struct Object {
    std::string bytes, content_type;
};

// The keys under `prefix` after `after`, at most `max`, with keys holding `delimiter` past the
// prefix rolled up into common prefixes: what both providers' listings do.
struct Listed {
    std::vector<std::string> keys, prefixes;
    std::string next;
};
inline Listed list_keys(const std::map<std::string, Object>& objects, const std::string& prefix,
                 const std::string& delimiter, const std::string& after, std::size_t max) {
    Listed out;
    std::set<std::string> rolled;
    std::string last;
    bool truncated = false;
    for (auto found = objects.upper_bound(after); found != objects.end(); ++found) {
        const std::string& key = found->first;
        if (!key.starts_with(prefix)) {
            continue;
        }
        std::string entry = key;
        bool common = false;
        if (!delimiter.empty()) {
            const std::size_t at = key.find(delimiter, prefix.size());
            if (at != std::string::npos) {
                entry = key.substr(0, at + delimiter.size());
                common = true;
            }
        }
        if (common && rolled.contains(entry)) {
            last = key;
            continue;
        }
        if (out.keys.size() + out.prefixes.size() >= max) {
            truncated = true;
            break;
        }
        if (common) {
            rolled.insert(entry);
            out.prefixes.push_back(entry);
        } else {
            out.keys.push_back(key);
        }
        last = key;
    }
    if (truncated) {
        out.next = last;
    }
    return out;
}

// ---- A fake S3 server ----

class FakeS3 {
public:
    std::string bucket = "media";
    std::string access_key_id = "AKIDEXAMPLE";
    std::string secret_access_key = "secret-for-tests";
    std::string region = "auto";
    bool virtual_host = false;
    std::map<std::string, Object> objects;
    int requests = 0;
    std::vector<std::string> signed_failures;

    lake::control::StoreHttp http() {
        return [this](StoreHttpRequest request) -> liquid::runtime::task<StoreHttpResponse> {
            co_return handle(request);
        };
    }

    lake::control::S3Bucket client(std::string endpoint, std::string prefix = {}) const {
        lake::control::S3Bucket out;
        out.endpoint = std::move(endpoint);
        out.region = region;
        out.bucket = bucket;
        out.prefix = std::move(prefix);
        out.access_key_id = access_key_id;
        out.secret_access_key = secret_access_key;
        out.virtual_host = virtual_host;
        return out;
    }

    StoreHttpResponse handle(const StoreHttpRequest& request) {
        ++requests;
        const Request url = parse(request.url);
        if (!signed_correctly(request, url)) {
            return answer(403, "<Error><Code>SignatureDoesNotMatch</Code><Message>no</Message></Error>");
        }
        std::string key;
        if (virtual_host) {
            if (!url.host.starts_with(bucket + ".")) {
                return answer(404, "<Error><Code>NoSuchBucket</Code></Error>");
            }
            key = decode(url.path.substr(1));
        } else {
            if (url.path != "/" + bucket && !url.path.starts_with("/" + bucket + "/")) {
                return answer(404, "<Error><Code>NoSuchBucket</Code></Error>");
            }
            key = url.path.size() > bucket.size() + 2 ? decode(url.path.substr(bucket.size() + 2)) : "";
        }
        const bool bucket_level = key.empty() && (url.path == "/" || url.path == "/" + bucket);
        if (request.method == "GET" && bucket_level) {
            return list(url);
        }
        if (request.method == "GET") {
            const auto found = objects.find(key);
            if (found == objects.end()) {
                return answer(404, "<Error><Code>NoSuchKey</Code></Error>");
            }
            const std::string range = header(request, "range");
            if (range.empty()) {
                return answer(200, found->second.bytes);
            }
            const std::size_t dash = range.find('-');
            const std::uint64_t first = std::stoull(range.substr(6, dash - 6));
            const std::uint64_t last = std::stoull(range.substr(dash + 1));
            if (first >= found->second.bytes.size()) {
                return answer(416, "<Error><Code>InvalidRange</Code></Error>");
            }
            return answer(206, found->second.bytes.substr(first, last - first + 1));
        }
        if (request.method == "PUT") {
            if (const std::string source = header(request, "x-amz-copy-source"); !source.empty()) {
                const std::string from = decode(source.substr(bucket.size() + 2));
                const auto found = objects.find(from);
                if (!source.starts_with("/" + bucket + "/") || found == objects.end()) {
                    return answer(404, "<Error><Code>NoSuchKey</Code></Error>");
                }
                objects[key] = found->second;
                return answer(200, "<CopyObjectResult><ETag>&quot;c&quot;</ETag></CopyObjectResult>");
            }
            objects[key] = {request.body, header(request, "content-type")};
            return answer(200, "", {{"etag", "\"" + sha256_hex(request.body).substr(0, 32) + "\""}});
        }
        if (request.method == "DELETE") {
            objects.erase(key);
            return answer(204);
        }
        return answer(405);
    }

private:
    bool signed_correctly(const StoreHttpRequest& request, const Request& url) {
        lake::control::SigV4Request again;
        again.method = request.method;
        again.host = url.host;
        again.path = url.path;
        again.query = url.query;
        again.payload_sha256 = header(request, "x-amz-content-sha256");
        for (const auto& [name, value] : request.headers) {
            if (name != "x-amz-date" && name != "x-amz-content-sha256" && name != "authorization" &&
                name != "x-amz-security-token") {
                again.headers.emplace_back(name, value);
            }
        }
        if (again.payload_sha256 != sha256_hex(request.body)) {
            signed_failures.push_back("payload");
            return false;
        }
        const std::string stamp = header(request, "x-amz-date");
        const auto when = at(std::stoi(stamp.substr(0, 4)), std::stoi(stamp.substr(4, 2)),
                             std::stoi(stamp.substr(6, 2)), std::stoi(stamp.substr(9, 2)));
        const auto expected = lake::control::sigv4_sign(
            again, {access_key_id, secret_access_key, "", region}, when);
        for (const auto& [name, value] : expected) {
            if (name == "authorization" && value == header(request, "authorization")) {
                return true;
            }
        }
        signed_failures.push_back(request.method + " " + request.url);
        return false;
    }

    StoreHttpResponse list(const Request& url) {
        if (url.param("list-type") != "2") {
            return answer(400, "<Error><Code>InvalidArgument</Code></Error>");
        }
        const bool encode = url.param("encoding-type") == "url";
        const auto key_text = [&](const std::string& key) {
            if (!encode) {
                return xml_escape(key);
            }
            std::string out = lake::control::sigv4_encode(key, true);
            std::string spaced;
            for (std::size_t index = 0; index < out.size(); ++index) {
                if (out.compare(index, 3, "%20") == 0) {
                    spaced += '+';
                    index += 2;
                } else {
                    spaced += out[index];
                }
            }
            return spaced;
        };
        const Listed listed = list_keys(objects, url.param("prefix"), url.param("delimiter"),
                                        url.param("continuation-token"),
                                        std::stoul(url.param("max-keys").empty() ? "1000" : url.param("max-keys")));
        std::string body = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><ListBucketResult>";
        if (encode) {
            body += "<EncodingType>url</EncodingType>";
        }
        for (const std::string& key : listed.keys) {
            body += "<Contents><Key>" + key_text(key) +
                    "</Key><LastModified>2026-10-11T12:00:00.000Z</LastModified><ETag>&quot;" +
                    sha256_hex(objects.at(key).bytes).substr(0, 32) + "&quot;</ETag><Size>" +
                    std::to_string(objects.at(key).bytes.size()) + "</Size></Contents>";
        }
        for (const std::string& prefix : listed.prefixes) {
            body += "<CommonPrefixes><Prefix>" + key_text(prefix) + "</Prefix></CommonPrefixes>";
        }
        body += std::string("<IsTruncated>") + (listed.next.empty() ? "false" : "true") + "</IsTruncated>";
        if (!listed.next.empty()) {
            body += "<NextContinuationToken>" + xml_escape(listed.next) + "</NextContinuationToken>";
        }
        return answer(200, body + "</ListBucketResult>");
    }
};

// ---- A fake Azure Blob server ----

class FakeAzure {
public:
    std::string account = "devstoreaccount1";
    std::string container = "photos";
    std::string account_key =
        "Eby8vdM02xNOcqFlqUwJPLlmEtlCDXJ1OUzFT50uSRZ6IFsuFq2UVErCz4I6tq/K1SZFPTOtr/KBHBeksoGMGw==";
    std::string sas; // when set, requests must carry it instead of a Shared Key
    std::map<std::string, Object> objects;
    std::set<std::string> copying; // blobs whose copy is still pending once
    int requests = 0;

    lake::control::StoreHttp http() {
        return [this](StoreHttpRequest request) -> liquid::runtime::task<StoreHttpResponse> {
            co_return handle(request);
        };
    }

    std::string endpoint() const {
        return "https://" + account + ".blob.core.windows.net";
    }

    StoreHttpResponse handle(const StoreHttpRequest& request) {
        ++requests;
        const Request url = parse(request.url);
        if (!authorized(request, url)) {
            return answer(403, "<Error><Code>AuthenticationFailed</Code><Message>no</Message></Error>");
        }
        if (!url.path.starts_with("/" + container)) {
            return answer(404, "<Error><Code>ContainerNotFound</Code></Error>");
        }
        const std::string key =
            url.path.size() > container.size() + 2 ? decode(url.path.substr(container.size() + 2)) : "";
        if (request.method == "GET" && key.empty() && url.param("comp") == "list") {
            return list(url);
        }
        if (request.method == "GET") {
            const auto found = objects.find(key);
            if (found == objects.end()) {
                return answer(404, "<Error><Code>BlobNotFound</Code></Error>");
            }
            const std::string range = header(request, "x-ms-range");
            if (range.empty()) {
                return answer(200, found->second.bytes);
            }
            const std::size_t dash = range.find('-');
            const std::uint64_t first = std::stoull(range.substr(6, dash - 6));
            const std::uint64_t last = std::stoull(range.substr(dash + 1));
            if (first >= found->second.bytes.size()) {
                return answer(416, "<Error><Code>InvalidRange</Code></Error>");
            }
            return answer(206, found->second.bytes.substr(first, last - first + 1));
        }
        if (request.method == "PUT") {
            if (const std::string source = header(request, "x-ms-copy-source"); !source.empty()) {
                const Request from = parse(source);
                const std::string name = decode(from.path.substr(container.size() + 2));
                const auto found = objects.find(name);
                if (found == objects.end()) {
                    return answer(404, "<Error><Code>CannotVerifyCopySource</Code></Error>");
                }
                if (!sas.empty() && from.param("sig") != sas) {
                    return answer(403, "<Error><Code>CannotVerifyCopySource</Code></Error>");
                }
                objects[key] = found->second;
                copying.insert(key);
                return answer(202, "", {{"x-ms-copy-status", "pending"}});
            }
            if (header(request, "x-ms-blob-type") != "BlockBlob") {
                return answer(400, "<Error><Code>MissingRequiredHeader</Code></Error>");
            }
            objects[key] = {request.body, header(request, "content-type")};
            return answer(201, "", {{"etag", "\"0x1\""}});
        }
        if (request.method == "DELETE") {
            if (objects.erase(key) == 0) {
                return answer(404, "<Error><Code>BlobNotFound</Code></Error>");
            }
            return answer(202);
        }
        return answer(405);
    }

private:
    bool authorized(const StoreHttpRequest& request, const Request& url) {
        if (!sas.empty()) {
            return url.param("sig") == sas && header(request, "authorization").empty();
        }
        std::vector<std::pair<std::string, std::string>> headers = request.headers;
        std::erase_if(headers, [](const auto& pair) { return pair.first == "authorization"; });
        headers.emplace_back("content-length", std::to_string(request.body.size()));
        const std::string signature = lake::control::azure_shared_key_signature(
            account, account_key, request.method, url.path, url.query, headers);
        return header(request, "authorization") == "SharedKey " + account + ":" + signature;
    }

    StoreHttpResponse list(const Request& url) {
        const Listed listed =
            list_keys(objects, url.param("prefix"), url.param("delimiter"), url.param("marker"),
                      std::stoul(url.param("maxresults").empty() ? "5000" : url.param("maxresults")));
        const bool include_copy = url.param("include") == "copy";
        std::string body = "<?xml version=\"1.0\" encoding=\"utf-8\"?><EnumerationResults><Blobs>";
        for (const std::string& key : listed.keys) {
            std::string copy;
            if (include_copy) {
                copy = "<CopyStatus>" + std::string(copying.erase(key) ? "pending" : "success") +
                       "</CopyStatus>";
            }
            body += "<Blob><Name>" + xml_escape(key) +
                    "</Name><Properties><Last-Modified>Sun, 11 Oct 2026 12:00:00 GMT</Last-Modified>"
                    "<Etag>0x1</Etag><Content-Length>" +
                    std::to_string(objects.at(key).bytes.size()) + "</Content-Length><Content-Type>" +
                    xml_escape(objects.at(key).content_type) + "</Content-Type>" + copy +
                    "</Properties></Blob>";
        }
        for (const std::string& prefix : listed.prefixes) {
            body += "<BlobPrefix><Name>" + xml_escape(prefix) + "</Name></BlobPrefix>";
        }
        body += "</Blobs><NextMarker>" + xml_escape(listed.next) + "</NextMarker></EnumerationResults>";
        return answer(200, body);
    }
};

// ---- A liquid repository branch, in memory ----

class FakeBranch {
public:
    std::map<std::string, std::string> files;
    std::set<std::string> folders; // explicit (made by a Folder operation)
    int changes = 0;

    lake::api::RepositoryBranch branch() {
        return {[this] { return tree(); },
                [this](const std::string& path) -> lake::api::SyncStoreResult<lake::api::RepositoryFile> {
                    const auto found = files.find(path);
                    if (found == files.end()) {
                        return std::unexpected(lake::api::SyncStoreError{"not_found", "no file " + path});
                    }
                    lake::api::RepositoryFile out{path, "blob", found->second, found->second.size()};
                    return out;
                },
                [this](const std::string&, std::vector<lake::api::RepositoryWriteOperation> operations)
                    -> lake::api::SyncStoreResult<std::string> {
                    for (auto& operation : operations) {
                        apply(operation);
                    }
                    return "change-" + std::to_string(++changes);
                }};
    }

private:
    lake::api::SyncStoreResult<std::vector<lake::api::RepositoryTreeEntry>> tree() const {
        std::set<std::string> trees = folders;
        for (const auto& [path, bytes] : files) {
            for (std::string level = lake::control::store_parent(path); !level.empty();
                 level = lake::control::store_parent(level)) {
                trees.insert(level);
            }
        }
        std::vector<lake::api::RepositoryTreeEntry> out;
        for (const std::string& folder : trees) {
            out.push_back({.path = folder, .kind = "tree"});
            if (folders.contains(folder)) {
                out.push_back({.path = folder + "/.lake-folder", .kind = "blob"});
            }
        }
        for (const auto& [path, bytes] : files) {
            out.push_back({.path = path, .kind = "blob", .blob = "b" + std::to_string(bytes.size()),
                           .size = bytes.size(), .last_changed_at = "2026-10-11T12:00:00Z"});
        }
        return out;
    }

    void apply(lake::api::RepositoryWriteOperation& operation) {
        using Kind = lake::api::RepositoryWriteKind;
        const auto inside = [](const std::string& path, const std::string& folder) {
            return path == folder || path.starts_with(folder + "/");
        };
        switch (operation.kind) {
        case Kind::File:
            files[operation.path] =
                std::string(reinterpret_cast<const char*>(operation.contents.data()), operation.contents.size());
            break;
        case Kind::Folder:
            folders.insert(operation.path);
            break;
        case Kind::Moved: {
            std::map<std::string, std::string> next;
            for (auto& [path, bytes] : files) {
                next[inside(path, operation.source_path)
                         ? operation.path + path.substr(operation.source_path.size())
                         : path] = std::move(bytes);
            }
            files = std::move(next);
            std::set<std::string> moved;
            for (const std::string& folder : folders) {
                moved.insert(inside(folder, operation.source_path)
                                 ? operation.path + folder.substr(operation.source_path.size())
                                 : folder);
            }
            folders = std::move(moved);
            break;
        }
        case Kind::DeletedFile:
            files.erase(operation.path);
            break;
        case Kind::DeletedFolder:
            std::erase_if(files, [&](const auto& file) { return inside(file.first, operation.path); });
            std::erase_if(folders, [&](const std::string& folder) { return inside(folder, operation.path); });
            break;
        default:
            break;
        }
    }
};

} // namespace lake::test::stores
