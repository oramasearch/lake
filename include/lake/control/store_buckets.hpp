#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <liquid/runtime.hpp>

#include "lake/control/store_contents.hpp"

namespace lake::control {

// A customer's own bucket browsed as a drive (docs/api/storage.md#browsing-a-bucket, lake#664),
// over the provider's own protocol: Lake's own S3 client (SigV4) for Cloudflare R2, AWS S3,
// Backblaze B2 and Google Cloud Storage's XML API, and its own Azure Blob client (Shared Key or
// a SAS). No provider SDK. Both speak through StoreHttp, the server's outbound transport, which
// reaches public addresses only.

// One HTTP exchange. Header names in the response are lower-cased.
struct StoreHttpRequest {
    std::string method = "GET";
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::chrono::milliseconds timeout{60'000};
    std::size_t max_response_bytes = 8 * 1024 * 1024;
};
struct StoreHttpResponse {
    int status = 0;
    std::map<std::string, std::string> headers;
    std::string body;
    std::string error; // the transport's failure; empty when an HTTP answer arrived
    std::string header(std::string_view name) const;
};
// Injected: the HTTP server's outbound transport, or a test's fake of the provider.
using StoreHttp = std::function<liquid::runtime::task<StoreHttpResponse>(StoreHttpRequest)>;
// The current time, injected so signatures are reproducible in tests.
using StoreClock = std::function<std::chrono::system_clock::time_point()>;

// ---- S3 (SigV4) ---------------------------------------------------------------------------------

struct S3Bucket {
    std::string endpoint; // https://host[:port], no path
    std::string region;   // "auto" for R2
    std::string bucket;
    std::string prefix; // the folder in the bucket the store covers, "" for all of it
    std::string access_key_id, secret_access_key, session_token;
    // Address the bucket as <bucket>.<endpoint host> (AWS S3) rather than <endpoint>/<bucket>.
    bool virtual_host = false;
};

// One request to sign. `path` is the URL path as sent (already percent-encoded); `query` the
// decoded parameters; `headers` every header sent besides the ones the signer adds.
struct SigV4Request {
    std::string method;
    std::string host; // host[:port] as the Host header carries it
    std::string path;
    std::vector<std::pair<std::string, std::string>> query;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string payload_sha256; // lowercase hex
};
struct SigV4Credentials {
    std::string access_key_id, secret_access_key, session_token, region, service = "s3";
};
// The headers to add (x-amz-date, x-amz-content-sha256, x-amz-security-token when there is a
// session token, and Authorization), signing every header of the request and those.
std::vector<std::pair<std::string, std::string>>
sigv4_sign(const SigV4Request& request, const SigV4Credentials& credentials,
           std::chrono::system_clock::time_point now);
// RFC 3986 percent-encoding as SigV4 wants it: unreserved characters kept, every other byte
// %XX in uppercase; "/" kept when `keep_slash`.
std::string sigv4_encode(std::string_view text, bool keep_slash);

std::shared_ptr<StoreContents> s3_store_contents(S3Bucket bucket, StoreHttp http,
                                                 StoreClock clock = {});

// ---- Azure Blob (Shared Key or SAS) -------------------------------------------------------------

struct AzureContainer {
    std::string endpoint; // https://<account>.blob.core.windows.net
    std::string account;  // from the endpoint's host when empty
    std::string container;
    std::string prefix;
    std::string account_key; // base64, for Shared Key
    std::string sas_token;   // a SAS query string, used instead when set
};

// The Shared Key signature of one request (base64 HMAC-SHA256 of the string to sign), for
// `Authorization: SharedKey <account>:<signature>`. `path` is the URL path as sent; `query` the
// decoded parameters; `headers` every header sent (x-ms-date and x-ms-version included).
std::string azure_shared_key_signature(std::string_view account,
                                       std::string_view account_key,
                                       std::string_view method,
                                       std::string_view path,
                                       const std::vector<std::pair<std::string, std::string>>& query,
                                       const std::vector<std::pair<std::string, std::string>>& headers);

std::shared_ptr<StoreContents> azure_store_contents(AzureContainer container, StoreHttp http,
                                                    StoreClock clock = {});

// ---- The browser ---------------------------------------------------------------------------------

// Opens a customer's own bucket from its store (bucket, endpoint, region, prefix) and the
// workspace secret's fields: `access_key_id`, `secret_access_key` and optionally `session_token`
// for the S3 protocol; `account_key` or `sas_token` for Azure. A missing field is
// `invalid_input` naming it.
StoreBrowser cloud_store_browser(StoreHttp http, StoreClock clock = {});

// The default endpoint of a provider when the store names none (AWS S3 in its region, Google
// Cloud Storage's XML API); empty when the provider needs one (R2's account, B2's region, Azure's
// account).
std::string default_store_endpoint(std::string_view provider, std::string_view region);
// Why an endpoint cannot be a store's (https://host[:port] with no path, query or credentials),
// or empty.
std::string store_endpoint_problem(std::string_view endpoint);
// Why a bucket or container name cannot be one (S3: 3-63 lowercase letters, digits, '.', '-';
// Azure: 3-63 lowercase letters, digits, '-'), or empty.
std::string store_bucket_problem(std::string_view provider, std::string_view bucket);
// Why a prefix cannot be a store's (a store path: names joined by '/'), or empty.
std::string store_prefix_problem(std::string_view prefix);

} // namespace lake::control
