#include "lake/control/saml.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

namespace lake::control::saml {
namespace {

struct Element {
    std::size_t start;
    std::size_t content_start;
    std::size_t content_end;
    std::size_t end;
};

std::string local_name(std::string_view value) {
    const std::size_t colon = value.rfind(':');
    return std::string(value.substr(colon == std::string_view::npos ? 0 : colon + 1));
}

bool name_char(char value) {
    return std::isalnum(static_cast<unsigned char>(value)) != 0 || value == ':' || value == '_' ||
           value == '-';
}

std::expected<std::size_t, std::string> close_tag(std::string_view xml, std::size_t start) {
    char quote = '\0';
    for (std::size_t index = start; index < xml.size(); ++index) {
        if (quote != '\0') {
            if (xml[index] == quote) {
                quote = '\0';
            }
        } else if (xml[index] == '\'' || xml[index] == '"') {
            quote = xml[index];
        } else if (xml[index] == '>') {
            return index;
        }
    }
    return std::unexpected("SAML XML has an unterminated tag");
}

std::expected<Element, std::string>
element(std::string_view xml, std::string_view wanted, std::size_t from = 0) {
    for (std::size_t start = xml.find('<', from); start != std::string_view::npos;
         start = xml.find('<', start + 1)) {
        if (start + 1 >= xml.size() || xml[start + 1] == '/' || xml[start + 1] == '?' ||
            xml[start + 1] == '!') {
            continue;
        }
        std::size_t name_end = start + 1;
        while (name_end < xml.size() && name_char(xml[name_end])) {
            ++name_end;
        }
        if (local_name(xml.substr(start + 1, name_end - start - 1)) != wanted) {
            continue;
        }
        auto opening_end = close_tag(xml, start + 1);
        if (!opening_end) {
            return std::unexpected(opening_end.error());
        }
        int depth = 1;
        for (std::size_t cursor = *opening_end + 1; cursor < xml.size();) {
            const std::size_t next = xml.find('<', cursor);
            if (next == std::string_view::npos || (next + 1 < xml.size() && xml[next + 1] == '!')) {
                break;
            }
            auto end = close_tag(xml, next + 1);
            if (!end) {
                return std::unexpected(end.error());
            }
            const bool closing = next + 1 < xml.size() && xml[next + 1] == '/';
            const std::size_t child_start = next + (closing ? 2 : 1);
            std::size_t child_end = child_start;
            while (child_end < *end && name_char(xml[child_end])) {
                ++child_end;
            }
            if (local_name(xml.substr(child_start, child_end - child_start)) == wanted) {
                if (closing && --depth == 0) {
                    return Element{start, *opening_end + 1, next, *end + 1};
                }
                if (!closing && (*end == next || xml[*end - 1] != '/')) {
                    ++depth;
                }
            }
            cursor = *end + 1;
        }
        return std::unexpected("SAML XML has an unclosed element");
    }
    return std::unexpected("required SAML element is missing");
}

std::string decode_entities(std::string_view value) {
    std::string result;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const std::pair<std::string_view, char> entities[] = {
            {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
        bool matched = false;
        for (const auto& [encoded, decoded] : entities) {
            if (value.substr(index).starts_with(encoded)) {
                result += decoded;
                index += encoded.size() - 1;
                matched = true;
                break;
            }
        }
        if (!matched) {
            result += value[index];
        }
    }
    return result;
}

std::string escape(std::string_view value, bool attribute) {
    std::string result;
    for (char character : value) {
        if (character == '&')
            result += "&amp;";
        else if (character == '<')
            result += "&lt;";
        else if (character == '>' && !attribute)
            result += "&gt;";
        else if (character == '"' && attribute)
            result += "&quot;";
        else if (character == '\t' && attribute)
            result += "&#x9;";
        else if (character == '\n' && attribute)
            result += "&#xA;";
        else if (character == '\r')
            result += attribute ? "&#xD;" : "&#xD;";
        else
            result += character;
    }
    return result;
}

std::expected<std::vector<std::pair<std::string, std::string>>, std::string>
attributes(std::string_view tag) {
    std::vector<std::pair<std::string, std::string>> result;
    std::size_t index = 0;
    while (index < tag.size() && name_char(tag[index]))
        ++index;
    while (index < tag.size()) {
        while (index < tag.size() && std::isspace(static_cast<unsigned char>(tag[index])) != 0)
            ++index;
        if (index == tag.size() || tag[index] == '/')
            break;
        const std::size_t name_start = index;
        while (index < tag.size() && name_char(tag[index]))
            ++index;
        if (name_start == index || index == tag.size() || tag[index++] != '=') {
            return std::unexpected("SAML XML has an invalid attribute");
        }
        if (index == tag.size() || (tag[index] != '\'' && tag[index] != '"')) {
            return std::unexpected("SAML XML attribute is not quoted");
        }
        const char quote = tag[index++];
        const std::size_t value_start = index;
        while (index < tag.size() && tag[index] != quote)
            ++index;
        if (index == tag.size())
            return std::unexpected("SAML XML attribute is unterminated");
        result.emplace_back(
            std::string(tag.substr(name_start, index - value_start - name_start - 1)),
            decode_entities(tag.substr(value_start, index - value_start)));
        ++index;
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::expected<std::string, std::string> base64(std::string_view value) {
    if (value.empty() || value.size() > static_cast<std::size_t>(INT_MAX)) {
        return std::unexpected("SAML response is empty or too large");
    }
    std::string padded(value);
    while (padded.size() % 4 != 0)
        padded += '=';
    std::string decoded((padded.size() / 4) * 3, '\0');
    const int size = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(decoded.data()),
                                     reinterpret_cast<const unsigned char*>(padded.data()),
                                     static_cast<int>(padded.size()));
    if (size < 0)
        return std::unexpected("SAML response is not base64");
    decoded.resize(static_cast<std::size_t>(size));
    const std::size_t padding = padded.ends_with("==") ? 2 : padded.ends_with('=') ? 1 : 0;
    decoded.resize(decoded.size() - padding);
    return decoded;
}

std::expected<std::string, std::string> text(std::string_view xml, std::string_view name) {
    auto found = element(xml, name);
    if (!found)
        return std::unexpected(found.error());
    const std::string_view value =
        xml.substr(found->content_start, found->content_end - found->content_start);
    if (value.find('<') != std::string_view::npos)
        return std::unexpected("SAML value contains nested XML");
    return decode_entities(value);
}

} // namespace

std::expected<std::string, std::string> canonicalize_exclusive(std::string_view xml,
                                                               bool omit_signature) {
    if (xml.empty() || xml.find("<!") != std::string_view::npos ||
        xml.find("<?") != std::string_view::npos) {
        return std::unexpected("SAML XML construct is not accepted");
    }
    std::string result;
    int omit_depth = 0;
    for (std::size_t position = 0; position < xml.size();) {
        const std::size_t next = xml.find('<', position);
        if (next == std::string_view::npos) {
            if (omit_depth == 0)
                result += escape(decode_entities(xml.substr(position)), false);
            break;
        }
        if (omit_depth == 0)
            result += escape(decode_entities(xml.substr(position, next - position)), false);
        auto end = close_tag(xml, next + 1);
        if (!end)
            return std::unexpected(end.error());
        const bool closing = xml[next + 1] == '/';
        const std::size_t name_start = next + (closing ? 2 : 1);
        std::size_t name_end = name_start;
        while (name_end < *end && name_char(xml[name_end]))
            ++name_end;
        const std::string name(xml.substr(name_start, name_end - name_start));
        if (name.empty())
            return std::unexpected("SAML XML has an invalid tag");
        if (omit_signature && !closing && local_name(name) == "Signature")
            ++omit_depth;
        else if (omit_signature && closing && omit_depth > 0 && local_name(name) == "Signature")
            --omit_depth;
        else if (omit_depth == 0) {
            if (closing) {
                result += "</" + name + ">";
            } else {
                auto parsed = attributes(xml.substr(name_start, *end - name_start));
                if (!parsed)
                    return std::unexpected(parsed.error());
                result += "<" + name;
                for (const auto& [key, value] : *parsed)
                    result += " " + key + "=\"" + escape(value, true) + "\"";
                result += ">";
                if (xml[*end - 1] == '/')
                    result += "</" + name + ">";
            }
        }
        position = *end + 1;
    }
    if (omit_depth != 0)
        return std::unexpected("SAML Signature is unclosed");
    return result;
}

std::expected<Assertion, std::string> verify_response(std::string_view response_base64,
                                                      std::string_view certificate_pem,
                                                      std::string_view expected_issuer) {
    auto response = base64(response_base64);
    if (!response)
        return std::unexpected(response.error());
    auto assertion = element(*response, "Assertion");
    if (!assertion)
        return std::unexpected(assertion.error());
    const std::string assertion_xml =
        response->substr(assertion->start, assertion->end - assertion->start);
    const std::string_view xml = assertion_xml;
    auto signed_info = element(xml, "SignedInfo");
    auto digest_value = text(xml, "DigestValue");
    auto signature_value = text(xml, "SignatureValue");
    if (!signed_info || !digest_value || !signature_value ||
        xml.find("http://www.w3.org/2001/10/xml-exc-c14n#") == std::string_view::npos ||
        xml.find("rsa-sha256") == std::string_view::npos) {
        return std::unexpected("SAML assertion does not use the required XML signature algorithms");
    }
    auto canonical_assertion = canonicalize_exclusive(xml, true);
    auto provided_digest = base64(*digest_value);
    if (!canonical_assertion || !provided_digest) {
        return std::unexpected("SAML assertion canonicalization or digest is invalid");
    }
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_size = 0;
    EVP_Digest(canonical_assertion->data(),
               canonical_assertion->size(),
               digest,
               &digest_size,
               EVP_sha256(),
               nullptr);
    if (provided_digest->size() != digest_size ||
        CRYPTO_memcmp(provided_digest->data(), digest, digest_size) != 0) {
        return std::unexpected("SAML assertion digest does not match its signature");
    }
    auto canonical_info = canonicalize_exclusive(
        xml.substr(signed_info->start, signed_info->end - signed_info->start));
    auto signature = base64(*signature_value);
    BIO* bio = BIO_new_mem_buf(certificate_pem.data(), static_cast<int>(certificate_pem.size()));
    X509* certificate =
        bio == nullptr ? nullptr : PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    EVP_PKEY* key = certificate == nullptr ? nullptr : X509_get_pubkey(certificate);
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    const bool verified =
        canonical_info && signature && key != nullptr && context != nullptr &&
        EVP_DigestVerifyInit(context, nullptr, EVP_sha256(), nullptr, key) == 1 &&
        EVP_DigestVerifyUpdate(context, canonical_info->data(), canonical_info->size()) == 1 &&
        EVP_DigestVerifyFinal(context,
                              reinterpret_cast<const unsigned char*>(signature->data()),
                              signature->size()) == 1;
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(key);
    X509_free(certificate);
    BIO_free(bio);
    if (!verified)
        return std::unexpected("SAML assertion signature is invalid");
    auto issuer = text(xml, "Issuer");
    auto subject = text(xml, "NameID");
    if (!issuer || !subject || *issuer != expected_issuer)
        return std::unexpected("SAML assertion issuer is not accepted");
    Assertion result{.issuer = *issuer, .subject = *subject, .claims = nlohmann::json::object()};
    for (std::size_t search = 0;;) {
        auto attribute = element(xml, "Attribute", search);
        if (!attribute)
            break;
        const std::string_view opening =
            xml.substr(attribute->start + 1, attribute->content_start - attribute->start - 2);
        auto parsed = attributes(opening);
        if (!parsed)
            return std::unexpected(parsed.error());
        auto value =
            text(xml.substr(attribute->start, attribute->end - attribute->start), "AttributeValue");
        const auto name = std::find_if(
            parsed->begin(), parsed->end(), [](const auto& item) { return item.first == "Name"; });
        if (value && name != parsed->end()) {
            result.claims[name->second] = *value;
            if (name->second == "email")
                result.email = *value;
            if (name->second == "name")
                result.display_name = *value;
        }
        search = attribute->end;
    }
    return result;
}

} // namespace lake::control::saml
