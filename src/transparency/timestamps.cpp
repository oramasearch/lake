// SPDX-License-Identifier: MIT

#include "lake/transparency/timestamps.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <ctime>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <openssl/asn1.h>
#include <openssl/asn1t.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/pkcs7.h>
#include <openssl/rand.h>
#include <openssl/ts.h>
#include <openssl/x509.h>

namespace lake::transparency {
namespace {

typedef struct lake_archive_timestamp_st {
    X509_ALGOR* digest_algorithm;
    PKCS7* time_stamp;
} LAKE_ARCHIVE_TIMESTAMP;

DEFINE_STACK_OF(LAKE_ARCHIVE_TIMESTAMP)

ASN1_SEQUENCE(LAKE_ARCHIVE_TIMESTAMP) =
    {
        ASN1_EXP_OPT(LAKE_ARCHIVE_TIMESTAMP, digest_algorithm, X509_ALGOR, 0),
        ASN1_SIMPLE(LAKE_ARCHIVE_TIMESTAMP, time_stamp, PKCS7),
} ASN1_SEQUENCE_END(LAKE_ARCHIVE_TIMESTAMP)

        IMPLEMENT_ASN1_FUNCTIONS(LAKE_ARCHIVE_TIMESTAMP)

            typedef struct lake_archive_timestamp_chain_st {
    STACK_OF(LAKE_ARCHIVE_TIMESTAMP) * timestamps;
} LAKE_ARCHIVE_TIMESTAMP_CHAIN;

DEFINE_STACK_OF(LAKE_ARCHIVE_TIMESTAMP_CHAIN)

ASN1_SEQUENCE(LAKE_ARCHIVE_TIMESTAMP_CHAIN) =
    {
        ASN1_SEQUENCE_OF(LAKE_ARCHIVE_TIMESTAMP_CHAIN, timestamps, LAKE_ARCHIVE_TIMESTAMP),
} ASN1_SEQUENCE_END(LAKE_ARCHIVE_TIMESTAMP_CHAIN)

        IMPLEMENT_ASN1_FUNCTIONS(LAKE_ARCHIVE_TIMESTAMP_CHAIN)

            typedef struct lake_evidence_record_st {
    ASN1_INTEGER* version;
    STACK_OF(X509_ALGOR) * digest_algorithms;
    STACK_OF(LAKE_ARCHIVE_TIMESTAMP_CHAIN) * archive_timestamp_sequence;
} LAKE_EVIDENCE_RECORD;

ASN1_SEQUENCE(LAKE_EVIDENCE_RECORD) =
    {
        ASN1_SIMPLE(LAKE_EVIDENCE_RECORD, version, ASN1_INTEGER),
        ASN1_SEQUENCE_OF(LAKE_EVIDENCE_RECORD, digest_algorithms, X509_ALGOR),
        ASN1_SEQUENCE_OF(LAKE_EVIDENCE_RECORD,
                         archive_timestamp_sequence,
                         LAKE_ARCHIVE_TIMESTAMP_CHAIN),
} ASN1_SEQUENCE_END(LAKE_EVIDENCE_RECORD)

        IMPLEMENT_ASN1_FUNCTIONS(LAKE_EVIDENCE_RECORD)

            template <class T, auto Free>
            struct OpenSslDeleter {
    void operator()(T* value) const {
        Free(value);
    }
};

using Request = std::unique_ptr<TS_REQ, OpenSslDeleter<TS_REQ, TS_REQ_free>>;
using Response = std::unique_ptr<TS_RESP, OpenSslDeleter<TS_RESP, TS_RESP_free>>;
using VerifyContext =
    std::unique_ptr<TS_VERIFY_CTX, OpenSslDeleter<TS_VERIFY_CTX, TS_VERIFY_CTX_free>>;
using MessageImprint =
    std::unique_ptr<TS_MSG_IMPRINT, OpenSslDeleter<TS_MSG_IMPRINT, TS_MSG_IMPRINT_free>>;
using Algorithm = std::unique_ptr<X509_ALGOR, OpenSslDeleter<X509_ALGOR, X509_ALGOR_free>>;
using Integer = std::unique_ptr<ASN1_INTEGER, OpenSslDeleter<ASN1_INTEGER, ASN1_INTEGER_free>>;
using Store = std::unique_ptr<X509_STORE, OpenSslDeleter<X509_STORE, X509_STORE_free>>;
using Certificate = std::unique_ptr<X509, OpenSslDeleter<X509, X509_free>>;
using Bio = std::unique_ptr<BIO, OpenSslDeleter<BIO, BIO_free>>;
using TstInfo = std::unique_ptr<TS_TST_INFO, OpenSslDeleter<TS_TST_INFO, TS_TST_INFO_free>>;
using EvidenceRecord =
    std::unique_ptr<LAKE_EVIDENCE_RECORD,
                    OpenSslDeleter<LAKE_EVIDENCE_RECORD, LAKE_EVIDENCE_RECORD_free>>;
using ArchiveTimestampPtr =
    std::unique_ptr<LAKE_ARCHIVE_TIMESTAMP,
                    OpenSslDeleter<LAKE_ARCHIVE_TIMESTAMP, LAKE_ARCHIVE_TIMESTAMP_free>>;
using ArchiveChainPtr = std::unique_ptr<
    LAKE_ARCHIVE_TIMESTAMP_CHAIN,
    OpenSslDeleter<LAKE_ARCHIVE_TIMESTAMP_CHAIN, LAKE_ARCHIVE_TIMESTAMP_CHAIN_free>>;

struct TimestampToken {
    std::unique_ptr<PKCS7, OpenSslDeleter<PKCS7, PKCS7_free>> token;
};

struct TokenDetails {
    std::vector<unsigned char> imprint;
    std::chrono::system_clock::time_point generated_at;
    std::chrono::system_clock::time_point signer_expires_at;
    std::string policy;
    std::string token_sha256;
};

struct InspectedEvidence {
    TimestampEvidence evidence;
    std::chrono::system_clock::time_point renew_after;
};

std::string crypto_error(std::string_view prefix) {
    std::string result(prefix);
    for (unsigned long code = ERR_get_error(); code != 0; code = ERR_get_error()) {
        std::array<char, 256> detail{};
        ERR_error_string_n(code, detail.data(), detail.size());
        result += result == prefix ? ": " : "; ";
        result += detail.data();
    }
    return result;
}

bool valid_instance_name(std::string_view value) {
    return !value.empty() && value != "." && value != ".." &&
           value.find('/') == std::string_view::npos && value.find('\\') == std::string_view::npos;
}

bool valid_checkpoint_digest(std::string_view value) {
    return value.size() == 64 && std::ranges::all_of(value, [](unsigned char byte) {
               return std::isdigit(byte) != 0 || (byte >= 'a' && byte <= 'f');
           });
}

Result<std::vector<unsigned char>> digest(std::span<const unsigned char> input) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> output{};
    unsigned int size = 0;
    if (EVP_Digest(input.data(), input.size(), output.data(), &size, EVP_sha256(), nullptr) != 1) {
        return std::unexpected(Error{"crypto_error", crypto_error("could not calculate SHA-256")});
    }
    return std::vector<unsigned char>(output.begin(), output.begin() + size);
}

Result<std::vector<unsigned char>> digest(std::string_view input) {
    return digest(std::span<const unsigned char>(
        reinterpret_cast<const unsigned char*>(input.data()), input.size()));
}

std::string hex(std::span<const unsigned char> value) {
    std::string output;
    output.reserve(value.size() * 2);
    for (const unsigned char byte : value) {
        output += std::format("{:02x}", byte);
    }
    return output;
}

Result<std::string> base64url_encode(std::span<const unsigned char> value) {
    std::string encoded(((value.size() + 2) / 3) * 4, '\0');
    const int size = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
                                     value.data(),
                                     static_cast<int>(value.size()));
    if (size < 0) {
        return std::unexpected(Error{"crypto_error", "could not encode evidence record"});
    }
    encoded.resize(static_cast<std::size_t>(size));
    while (encoded.ends_with('=')) {
        encoded.pop_back();
    }
    std::replace(encoded.begin(), encoded.end(), '+', '-');
    std::replace(encoded.begin(), encoded.end(), '/', '_');
    return encoded;
}

Result<std::vector<unsigned char>> base64_decode(std::string value) {
    if (value.empty()) {
        return std::unexpected(Error{"invalid_input", "signature must be nonempty base64"});
    }
    const std::size_t first_padding = value.find('=');
    const std::size_t unpadded_size =
        first_padding == std::string::npos ? value.size() : first_padding;
    const std::size_t provided_padding = value.size() - unpadded_size;
    const std::size_t required_padding = (4 - unpadded_size % 4) % 4;
    if (!std::ranges::all_of(value.substr(0, unpadded_size),
                             [](unsigned char byte) {
                                 return std::isalnum(byte) != 0 || byte == '+' || byte == '/' ||
                                        byte == '-' || byte == '_';
                             }) ||
        !std::ranges::all_of(value.substr(unpadded_size),
                             [](unsigned char byte) { return byte == '='; }) ||
        provided_padding > 2 || unpadded_size % 4 == 1 ||
        (provided_padding != 0 && provided_padding != required_padding)) {
        return std::unexpected(Error{"invalid_input", "signature is not valid base64"});
    }
    std::replace(value.begin(), value.end(), '-', '+');
    std::replace(value.begin(), value.end(), '_', '/');
    value.resize(unpadded_size);
    const std::size_t padding = required_padding;
    value.append(padding, '=');
    std::vector<unsigned char> decoded((value.size() / 4) * 3);
    const int size = EVP_DecodeBlock(decoded.data(),
                                     reinterpret_cast<const unsigned char*>(value.data()),
                                     static_cast<int>(value.size()));
    if (size < 0 || static_cast<std::size_t>(size) < padding) {
        return std::unexpected(Error{"invalid_input", "signature is not valid base64"});
    }
    decoded.resize(static_cast<std::size_t>(size) - padding);
    if (decoded.empty()) {
        return std::unexpected(Error{"invalid_input", "signature must not be empty"});
    }
    return decoded;
}

Result<std::vector<unsigned char>> encode_request(TS_REQ* request) {
    const int size = i2d_TS_REQ(request, nullptr);
    if (size <= 0) {
        return std::unexpected(
            Error{"crypto_error", crypto_error("could not encode RFC 3161 request")});
    }
    std::vector<unsigned char> encoded(static_cast<std::size_t>(size));
    unsigned char* cursor = encoded.data();
    if (i2d_TS_REQ(request, &cursor) != size) {
        return std::unexpected(
            Error{"crypto_error", crypto_error("could not encode RFC 3161 request")});
    }
    return encoded;
}

Result<Request> create_request(std::span<const unsigned char> imprint) {
    Request request(TS_REQ_new());
    MessageImprint message_imprint(TS_MSG_IMPRINT_new());
    Algorithm algorithm(X509_ALGOR_new());
    Integer nonce(ASN1_INTEGER_new());
    std::uint64_t nonce_value = 0;
    if (!request || !message_imprint || !algorithm || !nonce ||
        RAND_bytes(reinterpret_cast<unsigned char*>(&nonce_value), sizeof(nonce_value)) != 1) {
        return std::unexpected(
            Error{"crypto_error", crypto_error("could not create RFC 3161 request")});
    }
    X509_ALGOR_set_md(algorithm.get(), EVP_sha256());
    if (TS_REQ_set_version(request.get(), 1) != 1 ||
        TS_MSG_IMPRINT_set_algo(message_imprint.get(), algorithm.release()) != 1 ||
        TS_MSG_IMPRINT_set_msg(message_imprint.get(),
                               const_cast<unsigned char*>(imprint.data()),
                               imprint.size()) != 1 ||
        TS_REQ_set_msg_imprint(request.get(), message_imprint.release()) != 1 ||
        ASN1_INTEGER_set_uint64(nonce.get(), nonce_value) != 1 ||
        TS_REQ_set_nonce(request.get(), nonce.get()) != 1 ||
        TS_REQ_set_cert_req(request.get(), 1) != 1) {
        return std::unexpected(
            Error{"crypto_error", crypto_error("could not create RFC 3161 request")});
    }
    return request;
}

Result<Store> trust_store(std::string_view trust_pem) {
    Store store(X509_STORE_new());
    if (!store) {
        return std::unexpected(
            Error{"crypto_error", crypto_error("could not create TSA trust store")});
    }
    if (trust_pem.empty()) {
        if (X509_STORE_set_default_paths(store.get()) != 1) {
            return std::unexpected(
                Error{"crypto_error", crypto_error("could not load system TSA trust roots")});
        }
        return store;
    }
    Bio input(BIO_new_mem_buf(trust_pem.data(), static_cast<int>(trust_pem.size())));
    if (!input) {
        return std::unexpected(
            Error{"crypto_error", crypto_error("could not read TSA trust root")});
    }
    bool added = false;
    for (;;) {
        Certificate certificate(PEM_read_bio_X509(input.get(), nullptr, nullptr, nullptr));
        if (!certificate) {
            ERR_clear_error();
            break;
        }
        if (X509_STORE_add_cert(store.get(), certificate.get()) != 1) {
            return std::unexpected(
                Error{"crypto_error", crypto_error("could not add TSA trust root")});
        }
        added = true;
    }
    if (!added) {
        return std::unexpected(
            Error{"invalid_input", "TSA trust value contains no PEM certificate"});
    }
    return store;
}

Result<TimestampToken> request_timestamp(std::span<const unsigned char> imprint,
                                         std::string_view endpoint,
                                         std::string_view trust_pem,
                                         const TsaSender& sender) {
    if (endpoint.empty()) {
        return std::unexpected(
            Error{"store_unavailable", "no RFC 3161 TSA endpoint is configured"});
    }
    if (!sender) {
        return std::unexpected(
            Error{"store_unavailable", "no RFC 3161 TSA transport is configured"});
    }
    auto request = create_request(imprint);
    if (!request) {
        return std::unexpected(request.error());
    }
    const auto encoded_request = encode_request(request->get());
    if (!encoded_request) {
        return std::unexpected(encoded_request.error());
    }
    const std::string request_body(reinterpret_cast<const char*>(encoded_request->data()),
                                   encoded_request->size());
    const TsaReply reply = sender(endpoint, request_body);
    if (!reply.error.empty()) {
        return std::unexpected(Error{"store_unavailable", "TSA request failed: " + reply.error});
    }
    if (reply.status != 200) {
        return std::unexpected(
            Error{"store_unavailable", "TSA returned HTTP status " + std::to_string(reply.status)});
    }
    const unsigned char* cursor = reinterpret_cast<const unsigned char*>(reply.body.data());
    Response response(d2i_TS_RESP(nullptr, &cursor, static_cast<long>(reply.body.size())));
    if (!response ||
        cursor != reinterpret_cast<const unsigned char*>(reply.body.data()) + reply.body.size()) {
        return std::unexpected(
            Error{"invalid_input", crypto_error("TSA returned an invalid RFC 3161 response")});
    }
    const TS_STATUS_INFO* status = TS_RESP_get_status_info(response.get());
    const long status_value =
        status == nullptr ? -1 : ASN1_INTEGER_get(TS_STATUS_INFO_get0_status(status));
    if (status_value != TS_STATUS_GRANTED && status_value != TS_STATUS_GRANTED_WITH_MODS) {
        return std::unexpected(Error{"invalid_input", "TSA refused the timestamp request"});
    }
    auto store = trust_store(trust_pem);
    if (!store) {
        return std::unexpected(store.error());
    }
    PKCS7* response_token = TS_RESP_get_token(response.get());
    X509* signer = nullptr;
    if (response_token == nullptr ||
        TS_RESP_verify_signature(response_token, nullptr, store->get(), &signer) != 1) {
        return std::unexpected(
            Error{"invalid_input", crypto_error("RFC 3161 token signature verification failed")});
    }
    VerifyContext verification(TS_REQ_to_TS_VERIFY_CTX(request->get(), nullptr));
    const int response_flags = TS_VFY_VERSION | TS_VFY_IMPRINT | TS_VFY_NONCE;
    if (!verification) {
        return std::unexpected(
            Error{"crypto_error", crypto_error("could not create RFC 3161 verification context")});
    }
    TS_VERIFY_CTX_set_flags(verification.get(), response_flags);
    if (TS_RESP_verify_response(verification.get(), response.get()) != 1) {
        return std::unexpected(
            Error{"invalid_input", crypto_error("RFC 3161 response verification failed")});
    }
    std::unique_ptr<PKCS7, OpenSslDeleter<PKCS7, PKCS7_free>> token(
        response_token == nullptr ? nullptr : PKCS7_dup(response_token));
    if (!token) {
        return std::unexpected(Error{"invalid_input", "RFC 3161 response has no timestamp token"});
    }
    return TimestampToken{std::move(token)};
}

Result<std::vector<unsigned char>> encode_archive_timestamp(LAKE_ARCHIVE_TIMESTAMP* value) {
    const int size = i2d_LAKE_ARCHIVE_TIMESTAMP(value, nullptr);
    if (size <= 0) {
        return std::unexpected(
            Error{"crypto_error", crypto_error("could not encode archive timestamp")});
    }
    std::vector<unsigned char> encoded(static_cast<std::size_t>(size));
    unsigned char* cursor = encoded.data();
    if (i2d_LAKE_ARCHIVE_TIMESTAMP(value, &cursor) != size) {
        return std::unexpected(
            Error{"crypto_error", crypto_error("could not encode archive timestamp")});
    }
    return encoded;
}

Result<std::vector<unsigned char>> encode_evidence_record(LAKE_EVIDENCE_RECORD* value) {
    const int size = i2d_LAKE_EVIDENCE_RECORD(value, nullptr);
    if (size <= 0) {
        return std::unexpected(
            Error{"crypto_error", crypto_error("could not encode RFC 4998 evidence record")});
    }
    std::vector<unsigned char> encoded(static_cast<std::size_t>(size));
    unsigned char* cursor = encoded.data();
    if (i2d_LAKE_EVIDENCE_RECORD(value, &cursor) != size) {
        return std::unexpected(
            Error{"crypto_error", crypto_error("could not encode RFC 4998 evidence record")});
    }
    return encoded;
}

Result<EvidenceRecord> decode_evidence_record(std::span<const unsigned char> encoded) {
    const unsigned char* cursor = encoded.data();
    EvidenceRecord record(
        d2i_LAKE_EVIDENCE_RECORD(nullptr, &cursor, static_cast<long>(encoded.size())));
    if (!record || cursor != encoded.data() + encoded.size()) {
        return std::unexpected(
            Error{"store_error", crypto_error("stored RFC 4998 evidence record is invalid")});
    }
    return record;
}

Result<ArchiveTimestampPtr> archive_timestamp(PKCS7* token) {
    ArchiveTimestampPtr value(LAKE_ARCHIVE_TIMESTAMP_new());
    Algorithm algorithm(X509_ALGOR_new());
    if (!value || !algorithm) {
        return std::unexpected(
            Error{"crypto_error", crypto_error("could not create archive timestamp")});
    }
    X509_ALGOR_set_md(algorithm.get(), EVP_sha256());
    value->digest_algorithm = algorithm.release();
    value->time_stamp = token == nullptr ? nullptr : PKCS7_dup(token);
    if (!value->time_stamp) {
        return std::unexpected(
            Error{"crypto_error", crypto_error("could not copy RFC 3161 timestamp token")});
    }
    return value;
}

Result<EvidenceRecord> initial_record(PKCS7* token) {
    EvidenceRecord record(LAKE_EVIDENCE_RECORD_new());
    Algorithm algorithm(X509_ALGOR_new());
    ArchiveChainPtr chain(LAKE_ARCHIVE_TIMESTAMP_CHAIN_new());
    auto timestamp = archive_timestamp(token);
    if (!record || !algorithm || !chain || !timestamp) {
        return std::unexpected(
            timestamp ? Error{"crypto_error", crypto_error("could not create evidence record")}
                      : timestamp.error());
    }
    if (ASN1_INTEGER_set(record->version, 1) != 1) {
        return std::unexpected(
            Error{"crypto_error", crypto_error("could not set evidence record version")});
    }
    X509_ALGOR_set_md(algorithm.get(), EVP_sha256());
    if (sk_X509_ALGOR_push(record->digest_algorithms, algorithm.get()) <= 0) {
        return std::unexpected(
            Error{"crypto_error", crypto_error("could not assemble evidence record")});
    }
    algorithm.release();
    if (sk_LAKE_ARCHIVE_TIMESTAMP_push(chain->timestamps, timestamp->get()) <= 0) {
        return std::unexpected(
            Error{"crypto_error", crypto_error("could not assemble evidence record")});
    }
    timestamp->release();
    if (sk_LAKE_ARCHIVE_TIMESTAMP_CHAIN_push(record->archive_timestamp_sequence, chain.get()) <=
        0) {
        return std::unexpected(
            Error{"crypto_error", crypto_error("could not assemble evidence record")});
    }
    chain.release();
    return record;
}

Result<std::chrono::system_clock::time_point> as_time_point(const ASN1_TIME* value) {
    std::tm parts{};
    if (value == nullptr || ASN1_TIME_to_tm(value, &parts) != 1) {
        return std::unexpected(Error{"store_error", "timestamp token contains an invalid time"});
    }
    const std::time_t seconds = timegm(&parts);
    return std::chrono::system_clock::from_time_t(seconds);
}

std::string format_time(std::chrono::system_clock::time_point value) {
    return std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(value));
}

Result<TokenDetails> token_details(PKCS7* token) {
    if (token == nullptr) {
        return std::unexpected(
            Error{"store_error", "evidence record has an empty timestamp token"});
    }
    Bio verified_content(BIO_new(BIO_s_mem()));
    if (!verified_content ||
        PKCS7_verify(token, nullptr, nullptr, nullptr, verified_content.get(), PKCS7_NOVERIFY) !=
            1) {
        return std::unexpected(
            Error{"store_error", crypto_error("timestamp token signature is invalid")});
    }
    TstInfo info(PKCS7_to_TS_TST_INFO(token));
    if (!info) {
        return std::unexpected(
            Error{"store_error", crypto_error("timestamp token has no TSTInfo")});
    }
    TS_MSG_IMPRINT* message_imprint = TS_TST_INFO_get_msg_imprint(info.get());
    X509_ALGOR* algorithm =
        message_imprint == nullptr ? nullptr : TS_MSG_IMPRINT_get_algo(message_imprint);
    ASN1_OCTET_STRING* message =
        message_imprint == nullptr ? nullptr : TS_MSG_IMPRINT_get_msg(message_imprint);
    const ASN1_OBJECT* algorithm_object = nullptr;
    if (algorithm != nullptr) {
        X509_ALGOR_get0(&algorithm_object, nullptr, nullptr, algorithm);
    }
    if (algorithm_object == nullptr || OBJ_obj2nid(algorithm_object) != NID_sha256 ||
        message == nullptr || ASN1_STRING_length(message) != 32) {
        return std::unexpected(
            Error{"store_error", "timestamp token must contain a SHA-256 message imprint"});
    }
    const auto generated_at = as_time_point(TS_TST_INFO_get_time(info.get()));
    if (!generated_at) {
        return std::unexpected(generated_at.error());
    }
    STACK_OF(X509)* signers = PKCS7_get0_signers(token, nullptr, 0);
    if (signers == nullptr || sk_X509_num(signers) != 1) {
        sk_X509_free(signers);
        return std::unexpected(Error{"store_error", "timestamp token must have one signer"});
    }
    X509* signer = sk_X509_value(signers, 0);
    const auto signer_expires_at = as_time_point(X509_get0_notAfter(signer));
    sk_X509_free(signers);
    if (!signer_expires_at) {
        return std::unexpected(signer_expires_at.error());
    }
    std::array<char, 128> policy{};
    ASN1_OBJECT* policy_object = TS_TST_INFO_get_policy_id(info.get());
    const int policy_size =
        policy_object == nullptr ? 0 : OBJ_obj2txt(policy.data(), policy.size(), policy_object, 1);
    if (policy_size <= 0 || static_cast<std::size_t>(policy_size) >= policy.size()) {
        return std::unexpected(Error{"store_error", "timestamp token has an invalid policy"});
    }
    const int token_size = i2d_PKCS7(token, nullptr);
    if (token_size <= 0) {
        return std::unexpected(
            Error{"store_error", crypto_error("could not encode timestamp token")});
    }
    std::vector<unsigned char> token_der(static_cast<std::size_t>(token_size));
    unsigned char* token_cursor = token_der.data();
    if (i2d_PKCS7(token, &token_cursor) != token_size) {
        return std::unexpected(
            Error{"store_error", crypto_error("could not encode timestamp token")});
    }
    const auto token_digest = digest(token_der);
    if (!token_digest) {
        return std::unexpected(token_digest.error());
    }
    const unsigned char* imprint_data = ASN1_STRING_get0_data(message);
    return TokenDetails{
        .imprint = std::vector<unsigned char>(imprint_data, imprint_data + 32),
        .generated_at = *generated_at,
        .signer_expires_at = *signer_expires_at,
        .policy = std::string(policy.data(), static_cast<std::size_t>(policy_size)),
        .token_sha256 = hex(*token_digest),
    };
}

Result<InspectedEvidence> inspect(std::string_view checkpoint_digest,
                                  LAKE_EVIDENCE_RECORD* record,
                                  std::span<const unsigned char> encoded,
                                  const TimestampPolicy& policy) {
    const auto valid_policy = validate_timestamp_policy(policy);
    if (!valid_policy) {
        return std::unexpected(valid_policy.error());
    }
    if (ASN1_INTEGER_get(record->version) != 1 ||
        sk_X509_ALGOR_num(record->digest_algorithms) != 1 ||
        sk_LAKE_ARCHIVE_TIMESTAMP_CHAIN_num(record->archive_timestamp_sequence) != 1) {
        return std::unexpected(Error{"store_error", "unsupported RFC 4998 evidence record shape"});
    }
    const ASN1_OBJECT* digest_object = nullptr;
    X509_ALGOR_get0(
        &digest_object, nullptr, nullptr, sk_X509_ALGOR_value(record->digest_algorithms, 0));
    if (digest_object == nullptr || OBJ_obj2nid(digest_object) != NID_sha256) {
        return std::unexpected(
            Error{"store_error", "unsupported evidence record digest algorithm"});
    }
    LAKE_ARCHIVE_TIMESTAMP_CHAIN* chain =
        sk_LAKE_ARCHIVE_TIMESTAMP_CHAIN_value(record->archive_timestamp_sequence, 0);
    const int count = chain == nullptr ? 0 : sk_LAKE_ARCHIVE_TIMESTAMP_num(chain->timestamps);
    if (count <= 0) {
        return std::unexpected(Error{"store_error", "evidence record has no archive timestamps"});
    }
    TimestampEvidence evidence;
    evidence.checkpoint_digest = checkpoint_digest;
    evidence.digest_algorithm = "sha256";
    TokenDetails latest;
    for (int index = 0; index < count; ++index) {
        LAKE_ARCHIVE_TIMESTAMP* timestamp =
            sk_LAKE_ARCHIVE_TIMESTAMP_value(chain->timestamps, index);
        if (timestamp == nullptr || timestamp->digest_algorithm == nullptr) {
            return std::unexpected(
                Error{"store_error", "archive timestamp has no digest algorithm"});
        }
        const ASN1_OBJECT* timestamp_algorithm = nullptr;
        X509_ALGOR_get0(&timestamp_algorithm, nullptr, nullptr, timestamp->digest_algorithm);
        if (timestamp_algorithm == nullptr || OBJ_obj2nid(timestamp_algorithm) != NID_sha256) {
            return std::unexpected(
                Error{"store_error", "unsupported archive timestamp digest algorithm"});
        }
        const auto details = token_details(timestamp->time_stamp);
        if (!details) {
            return std::unexpected(details.error());
        }
        if (index == 0) {
            evidence.signature_digest = hex(details->imprint);
        } else {
            LAKE_ARCHIVE_TIMESTAMP* predecessor =
                sk_LAKE_ARCHIVE_TIMESTAMP_value(chain->timestamps, index - 1);
            const auto predecessor_der = encode_archive_timestamp(predecessor);
            if (!predecessor_der) {
                return std::unexpected(predecessor_der.error());
            }
            const auto predecessor_digest = digest(*predecessor_der);
            if (!predecessor_digest) {
                return std::unexpected(predecessor_digest.error());
            }
            if (*predecessor_digest != details->imprint) {
                return std::unexpected(
                    Error{"store_error", "archive timestamp does not cover its predecessor"});
            }
        }
        evidence.archive_timestamps.push_back(
            ArchiveTimestamp{static_cast<std::uint64_t>(index + 1),
                             format_time(details->generated_at),
                             details->policy,
                             details->token_sha256});
        latest = *details;
    }
    const auto interval_due = latest.generated_at + policy.renewal_interval;
    const auto certificate_due = latest.signer_expires_at - policy.renew_before_expiry;
    const auto renew_after = std::min(interval_due, certificate_due);
    evidence.renew_after = format_time(renew_after);
    const auto encoded_base64 = base64url_encode(encoded);
    if (!encoded_base64) {
        return std::unexpected(encoded_base64.error());
    }
    evidence.evidence_record = *encoded_base64;
    return InspectedEvidence{std::move(evidence), renew_after};
}

Result<std::vector<unsigned char>> read_record(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::unexpected(Error{"not_found", "timestamp evidence does not exist"});
    }
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size <= 0 || size > 2 * 1024 * 1024) {
        return std::unexpected(
            Error{"store_error", "stored timestamp evidence has an invalid size"});
    }
    input.seekg(0, std::ios::beg);
    std::vector<unsigned char> encoded(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(encoded.data()), size);
    if (!input) {
        return std::unexpected(Error{"store_error", "could not read timestamp evidence"});
    }
    return encoded;
}

Result<void> write_record(const std::filesystem::path& path,
                          std::span<const unsigned char> encoded,
                          bool replace) {
    if (!replace && std::filesystem::exists(path)) {
        return std::unexpected(Error{"conflict", "checkpoint already has timestamp evidence"});
    }
    static std::atomic<std::uint64_t> next_temporary{0};
    const std::filesystem::path temporary =
        path.string() + ".tmp-" + std::to_string(++next_temporary);
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            return std::unexpected(Error{"store_error", "could not create timestamp evidence"});
        }
        output.write(reinterpret_cast<const char*>(encoded.data()), encoded.size());
        output.flush();
        if (!output) {
            std::filesystem::remove(temporary);
            return std::unexpected(Error{"store_error", "could not write timestamp evidence"});
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary);
        return std::unexpected(
            Error{"store_error", "could not publish timestamp evidence: " + error.message()});
    }
    return {};
}

} // namespace

Result<void> validate_timestamp_policy(const TimestampPolicy& policy) {
    if (policy.renewal_interval <= std::chrono::seconds::zero()) {
        return std::unexpected(
            Error{"invalid_input", "timestamp renewal interval must be positive"});
    }
    if (policy.renew_before_expiry < std::chrono::seconds::zero()) {
        return std::unexpected(
            Error{"invalid_input", "timestamp renewal expiry margin cannot be negative"});
    }
    return {};
}

struct TimestampService::Impl {
    std::filesystem::path directory;
    bool valid_instance = false;
    mutable std::mutex mutex;

    Impl(std::filesystem::path store_root, std::string instance_name) {
        valid_instance = valid_instance_name(instance_name);
        if (valid_instance) {
            directory = std::move(store_root) / std::move(instance_name) / "log" / "timestamps";
        }
    }

    std::filesystem::path path(std::string_view checkpoint_digest) const {
        return directory / (std::string(checkpoint_digest) + ".ers");
    }

    Result<InspectedEvidence> load(std::string_view checkpoint_digest,
                                   const TimestampPolicy& policy) const {
        if (!valid_checkpoint_digest(checkpoint_digest)) {
            return std::unexpected(Error{
                "invalid_input", "checkpoint_digest must be 64 lowercase SHA-256 hex digits"});
        }
        const auto encoded = read_record(path(checkpoint_digest));
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        const auto record = decode_evidence_record(*encoded);
        if (!record) {
            return std::unexpected(record.error());
        }
        return inspect(checkpoint_digest, record->get(), *encoded, policy);
    }
};

TimestampService::TimestampService(std::filesystem::path store_root, std::string instance_name)
    : impl_(std::make_shared<Impl>(std::move(store_root), std::move(instance_name))) {}

Result<TimestampEvidence> TimestampService::timestamp(std::string checkpoint_digest,
                                                      std::string_view signature_base64,
                                                      std::string_view tsa_endpoint,
                                                      std::string_view tsa_trust_pem,
                                                      const TimestampPolicy& policy,
                                                      const TsaSender& sender) {
    std::unique_lock lock(impl_->mutex);
    if (!impl_->valid_instance) {
        return std::unexpected(
            Error{"invalid_input", "Lake instance name is not a safe path component"});
    }
    if (!valid_checkpoint_digest(checkpoint_digest)) {
        return std::unexpected(
            Error{"invalid_input", "checkpoint_digest must be 64 lowercase SHA-256 hex digits"});
    }
    const auto valid_policy = validate_timestamp_policy(policy);
    if (!valid_policy) {
        return std::unexpected(valid_policy.error());
    }
    if (std::filesystem::exists(impl_->path(checkpoint_digest))) {
        return std::unexpected(Error{"conflict", "checkpoint already has timestamp evidence"});
    }
    const auto signature = base64_decode(std::string(signature_base64));
    if (!signature) {
        return std::unexpected(signature.error());
    }
    const auto signature_digest = digest(*signature);
    if (!signature_digest) {
        return std::unexpected(signature_digest.error());
    }
    const auto timestamp_token =
        request_timestamp(*signature_digest, tsa_endpoint, tsa_trust_pem, sender);
    if (!timestamp_token) {
        return std::unexpected(timestamp_token.error());
    }
    const auto record = initial_record(timestamp_token->token.get());
    if (!record) {
        return std::unexpected(record.error());
    }
    const auto encoded = encode_evidence_record(record->get());
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    std::error_code error;
    std::filesystem::create_directories(impl_->directory, error);
    if (error) {
        return std::unexpected(
            Error{"store_error", "could not create timestamp log: " + error.message()});
    }
    const auto written = write_record(impl_->path(checkpoint_digest), *encoded, false);
    if (!written) {
        return std::unexpected(written.error());
    }
    const auto inspected = inspect(checkpoint_digest, record->get(), *encoded, policy);
    if (!inspected) {
        return std::unexpected(inspected.error());
    }
    return inspected->evidence;
}

Result<TimestampEvidence> TimestampService::renew(std::string_view checkpoint_digest,
                                                  std::string_view tsa_endpoint,
                                                  std::string_view tsa_trust_pem,
                                                  const TimestampPolicy& policy,
                                                  const TsaSender& sender) {
    std::unique_lock lock(impl_->mutex);
    if (!impl_->valid_instance) {
        return std::unexpected(
            Error{"invalid_input", "Lake instance name is not a safe path component"});
    }
    if (!valid_checkpoint_digest(checkpoint_digest)) {
        return std::unexpected(
            Error{"invalid_input", "checkpoint_digest must be 64 lowercase SHA-256 hex digits"});
    }
    const auto encoded = read_record(impl_->path(checkpoint_digest));
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    auto record = decode_evidence_record(*encoded);
    if (!record) {
        return std::unexpected(record.error());
    }
    const auto inspected = inspect(checkpoint_digest, record->get(), *encoded, policy);
    if (!inspected) {
        return std::unexpected(inspected.error());
    }
    LAKE_ARCHIVE_TIMESTAMP_CHAIN* chain =
        sk_LAKE_ARCHIVE_TIMESTAMP_CHAIN_value(record.value()->archive_timestamp_sequence, 0);
    LAKE_ARCHIVE_TIMESTAMP* previous = sk_LAKE_ARCHIVE_TIMESTAMP_value(
        chain->timestamps, sk_LAKE_ARCHIVE_TIMESTAMP_num(chain->timestamps) - 1);
    const auto previous_der = encode_archive_timestamp(previous);
    if (!previous_der) {
        return std::unexpected(previous_der.error());
    }
    const auto previous_digest = digest(*previous_der);
    if (!previous_digest) {
        return std::unexpected(previous_digest.error());
    }
    const auto timestamp_token =
        request_timestamp(*previous_digest, tsa_endpoint, tsa_trust_pem, sender);
    if (!timestamp_token) {
        return std::unexpected(timestamp_token.error());
    }
    auto renewed = archive_timestamp(timestamp_token->token.get());
    if (!renewed) {
        return std::unexpected(renewed.error());
    }
    if (sk_LAKE_ARCHIVE_TIMESTAMP_push(chain->timestamps, renewed->get()) <= 0) {
        return std::unexpected(
            Error{"crypto_error", crypto_error("could not append archive timestamp")});
    }
    renewed->release();
    const auto renewed_encoded = encode_evidence_record(record->get());
    if (!renewed_encoded) {
        return std::unexpected(renewed_encoded.error());
    }
    const auto verified = inspect(checkpoint_digest, record->get(), *renewed_encoded, policy);
    if (!verified) {
        return std::unexpected(verified.error());
    }
    const auto written = write_record(impl_->path(checkpoint_digest), *renewed_encoded, true);
    if (!written) {
        return std::unexpected(written.error());
    }
    return verified->evidence;
}

Result<TimestampEvidence> TimestampService::evidence(std::string_view checkpoint_digest,
                                                     const TimestampPolicy& policy) const {
    std::unique_lock lock(impl_->mutex);
    if (!impl_->valid_instance) {
        return std::unexpected(
            Error{"invalid_input", "Lake instance name is not a safe path component"});
    }
    const auto loaded = impl_->load(checkpoint_digest, policy);
    if (!loaded) {
        return std::unexpected(loaded.error());
    }
    return loaded->evidence;
}

Result<std::vector<std::string>> TimestampService::due(std::chrono::system_clock::time_point now,
                                                       const TimestampPolicy& policy) const {
    std::unique_lock lock(impl_->mutex);
    if (!impl_->valid_instance) {
        return std::unexpected(
            Error{"invalid_input", "Lake instance name is not a safe path component"});
    }
    const auto valid_policy = validate_timestamp_policy(policy);
    if (!valid_policy) {
        return std::unexpected(valid_policy.error());
    }
    std::vector<std::string> result;
    std::error_code error;
    if (!std::filesystem::exists(impl_->directory, error)) {
        return result;
    }
    for (const auto& entry : std::filesystem::directory_iterator(impl_->directory, error)) {
        if (error) {
            return std::unexpected(
                Error{"store_error", "could not list timestamp evidence: " + error.message()});
        }
        if (!entry.is_regular_file() || entry.path().extension() != ".ers") {
            continue;
        }
        const std::string checkpoint_digest = entry.path().stem().string();
        if (!valid_checkpoint_digest(checkpoint_digest)) {
            continue;
        }
        const auto loaded = impl_->load(checkpoint_digest, policy);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        if (loaded->renew_after <= now) {
            result.push_back(checkpoint_digest);
        }
    }
    std::ranges::sort(result);
    return result;
}

const std::filesystem::path& TimestampService::directory() const {
    return impl_->directory;
}

} // namespace lake::transparency
