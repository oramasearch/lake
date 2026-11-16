// SPDX-License-Identifier: MIT

#include "lake/control/saml.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>

namespace lake::control::saml {
namespace {

constexpr std::string_view xml_namespace = "http://www.w3.org/XML/1998/namespace";
constexpr std::string_view xmlns_namespace = "http://www.w3.org/2000/xmlns/";
constexpr std::string_view dsig_namespace = "http://www.w3.org/2000/09/xmldsig#";
constexpr std::string_view protocol_namespace = "urn:oasis:names:tc:SAML:2.0:protocol";
constexpr std::string_view assertion_namespace = "urn:oasis:names:tc:SAML:2.0:assertion";
constexpr std::string_view metadata_namespace = "urn:oasis:names:tc:SAML:2.0:metadata";
constexpr std::string_view exclusive_c14n = "http://www.w3.org/2001/10/xml-exc-c14n#";
constexpr std::string_view enveloped_signature =
    "http://www.w3.org/2000/09/xmldsig#enveloped-signature";
constexpr std::string_view status_success = "urn:oasis:names:tc:SAML:2.0:status:Success";
constexpr std::string_view bearer_method = "urn:oasis:names:tc:SAML:2.0:cm:bearer";
constexpr std::string_view entity_format = "urn:oasis:names:tc:SAML:2.0:nameid-format:entity";
constexpr std::string_view post_binding = "urn:oasis:names:tc:SAML:2.0:bindings:HTTP-POST";
constexpr std::string_view redirect_binding = "urn:oasis:names:tc:SAML:2.0:bindings:HTTP-Redirect";

constexpr int maximum_depth = 64;
constexpr std::size_t maximum_elements = 20000;

// ---------------------------------------------------------------------------------------------
// A strict XML 1.0 + Namespaces reader. It accepts an optional XML declaration, comments
// (dropped, as canonicalization without comments drops them), elements, attributes, text, the
// five predefined entities and character references. It refuses DTDs, other entities, CDATA and
// processing instructions, so there is no entity expansion and no external fetch.
// ---------------------------------------------------------------------------------------------

struct Attribute {
    std::string qname;
    std::string prefix;
    std::string local;
    std::string ns;
    std::string value;
};

struct Node {
    bool is_text = false;
    std::string text;
    std::string qname;
    std::string prefix;
    std::string local;
    std::string ns;
    std::vector<Attribute> attributes;
    // Every namespace in scope here: prefix ("" is the default) to URI ("" undeclares the default).
    std::map<std::string, std::string> scope;
    std::vector<std::unique_ptr<Node>> children;
    const Node* parent = nullptr;
};

using Error = std::string;

std::string normalize_line_ends(std::string_view input) {
    std::string result;
    result.reserve(input.size());
    for (std::size_t index = 0; index < input.size(); ++index) {
        if (input[index] == '\r') {
            result += '\n';
            if (index + 1 < input.size() && input[index + 1] == '\n') {
                ++index;
            }
        } else {
            result += input[index];
        }
    }
    return result;
}

bool name_start(unsigned char value) {
    return std::isalpha(value) != 0 || value == '_' || value == ':' || value >= 0x80;
}

bool name_char(unsigned char value) {
    return name_start(value) || std::isdigit(value) != 0 || value == '-' || value == '.';
}

bool xml_space(char value) {
    return value == ' ' || value == '\t' || value == '\n';
}

void append_utf8(std::string& out, std::uint32_t code) {
    if (code < 0x80) {
        out += static_cast<char>(code);
    } else if (code < 0x800) {
        out += static_cast<char>(0xC0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
        out += static_cast<char>(0xE0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (code >> 18));
        out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
}

bool xml_char(std::uint32_t code) {
    return code == 0x9 || code == 0xA || code == 0xD || (code >= 0x20 && code <= 0xD7FF) ||
           (code >= 0xE000 && code <= 0xFFFD) || (code >= 0x10000 && code <= 0x10FFFF);
}

class Parser {
public:
    explicit Parser(std::string input) : input_(std::move(input)) {}

    std::expected<std::unique_ptr<Node>, Error> document() {
        if (input_.starts_with("\xEF\xBB\xBF")) {
            position_ = 3;
        }
        if (starts("<?xml") && position_ + 5 < input_.size() && xml_space(input_[position_ + 5])) {
            const std::size_t end = input_.find("?>", position_);
            if (end == std::string::npos) {
                return std::unexpected("the XML declaration is unterminated");
            }
            position_ = end + 2;
        }
        if (auto misc = skip_misc(); !misc) {
            return std::unexpected(misc.error());
        }
        if (!starts("<")) {
            return std::unexpected("the document has no root element");
        }
        auto root = element(nullptr, 1);
        if (!root) {
            return std::unexpected(root.error());
        }
        if (auto misc = skip_misc(); !misc) {
            return std::unexpected(misc.error());
        }
        if (position_ != input_.size()) {
            return std::unexpected("the document has content after its root element");
        }
        return root;
    }

private:
    bool starts(std::string_view value) const {
        return std::string_view(input_).substr(position_).starts_with(value);
    }

    void skip_space() {
        while (position_ < input_.size() && xml_space(input_[position_])) {
            ++position_;
        }
    }

    std::expected<void, Error> skip_comment() {
        const std::size_t end = input_.find("-->", position_ + 4);
        if (end == std::string::npos) {
            return std::unexpected("an XML comment is unterminated");
        }
        position_ = end + 3;
        return {};
    }

    std::expected<void, Error> skip_misc() {
        for (;;) {
            skip_space();
            if (starts("<!--")) {
                if (auto skipped = skip_comment(); !skipped) {
                    return skipped;
                }
            } else if (starts("<!")) {
                return std::unexpected("document type declarations are not accepted");
            } else if (starts("<?")) {
                return std::unexpected("processing instructions are not accepted");
            } else {
                return {};
            }
        }
    }

    std::expected<std::string, Error> name() {
        const std::size_t start = position_;
        if (position_ >= input_.size() ||
            !name_start(static_cast<unsigned char>(input_[position_]))) {
            return std::unexpected("an XML name is invalid");
        }
        while (position_ < input_.size() &&
               name_char(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
        return input_.substr(start, position_ - start);
    }

    // At '&': the five predefined entities and character references, nothing else.
    std::expected<void, Error> reference(std::string& out) {
        const std::size_t end = input_.find(';', position_);
        if (end == std::string::npos || end - position_ > 12) {
            return std::unexpected("an XML reference is unterminated");
        }
        const std::string_view body =
            std::string_view(input_).substr(position_ + 1, end - position_ - 1);
        position_ = end + 1;
        static constexpr std::array<std::pair<std::string_view, char>, 5> predefined{
            {{"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}}};
        for (const auto& [entity, value] : predefined) {
            if (body == entity) {
                out += value;
                return {};
            }
        }
        if (body.size() < 2 || body[0] != '#') {
            return std::unexpected("XML entities other than the predefined five are not accepted");
        }
        const bool hex = body[1] == 'x';
        const std::string_view digits = body.substr(hex ? 2 : 1);
        if (digits.empty()) {
            return std::unexpected("an XML character reference is invalid");
        }
        std::uint32_t code = 0;
        for (const char digit : digits) {
            int value = -1;
            if (digit >= '0' && digit <= '9') {
                value = digit - '0';
            } else if (hex && digit >= 'a' && digit <= 'f') {
                value = digit - 'a' + 10;
            } else if (hex && digit >= 'A' && digit <= 'F') {
                value = digit - 'A' + 10;
            }
            if (value < 0 || code > 0x10FFFF) {
                return std::unexpected("an XML character reference is invalid");
            }
            code = code * (hex ? 16 : 10) + static_cast<std::uint32_t>(value);
        }
        if (!xml_char(code)) {
            return std::unexpected("an XML character reference is invalid");
        }
        append_utf8(out, code);
        return {};
    }

    static std::expected<std::pair<std::string, std::string>, Error> split(std::string_view qname) {
        const std::size_t colon = qname.find(':');
        if (colon == std::string_view::npos) {
            return std::pair<std::string, std::string>{"", std::string(qname)};
        }
        if (colon == 0 || colon + 1 == qname.size() ||
            qname.find(':', colon + 1) != std::string_view::npos) {
            return std::unexpected("an XML qualified name is invalid");
        }
        return std::pair<std::string, std::string>{std::string(qname.substr(0, colon)),
                                                   std::string(qname.substr(colon + 1))};
    }

    static std::expected<std::string, Error> resolve(const Node& node, const std::string& prefix) {
        if (prefix == "xml") {
            return std::string(xml_namespace);
        }
        const auto found = node.scope.find(prefix);
        if (prefix.empty()) {
            return found == node.scope.end() ? std::string() : found->second;
        }
        if (found == node.scope.end() || found->second.empty()) {
            return std::unexpected("the XML prefix " + prefix + " is not declared");
        }
        return found->second;
    }

    std::expected<std::unique_ptr<Node>, Error> element(const Node* parent, int depth) {
        if (depth > maximum_depth) {
            return std::unexpected("the XML document is nested too deeply");
        }
        if (++elements_ > maximum_elements) {
            return std::unexpected("the XML document has too many elements");
        }
        ++position_; // '<'
        auto qname = name();
        if (!qname) {
            return std::unexpected(qname.error());
        }
        auto node = std::make_unique<Node>();
        node->qname = *qname;
        node->parent = parent;
        if (parent != nullptr) {
            node->scope = parent->scope;
        }
        std::vector<std::pair<std::string, std::string>> raw;
        bool empty = false;
        for (;;) {
            const std::size_t before = position_;
            skip_space();
            if (position_ >= input_.size()) {
                return std::unexpected("an XML start tag is unterminated");
            }
            if (starts("/>")) {
                position_ += 2;
                empty = true;
                break;
            }
            if (input_[position_] == '>') {
                ++position_;
                break;
            }
            if (position_ == before) {
                return std::unexpected("XML attributes must be separated by white space");
            }
            auto attribute_name = name();
            if (!attribute_name) {
                return std::unexpected(attribute_name.error());
            }
            skip_space();
            if (!starts("=")) {
                return std::unexpected("an XML attribute has no value");
            }
            ++position_;
            skip_space();
            if (position_ >= input_.size() ||
                (input_[position_] != '"' && input_[position_] != '\'')) {
                return std::unexpected("an XML attribute value is not quoted");
            }
            const char quote = input_[position_++];
            std::string value;
            for (;;) {
                if (position_ >= input_.size()) {
                    return std::unexpected("an XML attribute value is unterminated");
                }
                const char character = input_[position_];
                if (character == quote) {
                    ++position_;
                    break;
                }
                if (character == '<') {
                    return std::unexpected("an XML attribute value contains '<'");
                }
                if (character == '&') {
                    if (auto decoded = reference(value); !decoded) {
                        return std::unexpected(decoded.error());
                    }
                    continue;
                }
                if (static_cast<unsigned char>(character) < 0x20 && character != '\t' &&
                    character != '\n') {
                    return std::unexpected("an XML attribute value contains a control character");
                }
                // Attribute-value normalization: literal white space becomes a space.
                value += xml_space(character) ? ' ' : character;
                ++position_;
            }
            if (std::ranges::any_of(
                    raw, [&](const auto& item) { return item.first == *attribute_name; })) {
                return std::unexpected("an XML element repeats an attribute");
            }
            raw.emplace_back(*attribute_name, std::move(value));
        }
        for (const auto& [attribute_name, value] : raw) {
            if (attribute_name == "xmlns") {
                if (value == xml_namespace || value == xmlns_namespace) {
                    return std::unexpected("a reserved XML namespace is redeclared");
                }
                node->scope[""] = value;
            } else if (attribute_name.starts_with("xmlns:")) {
                const std::string prefix = attribute_name.substr(6);
                if (prefix.empty() || prefix.find(':') != std::string::npos || value.empty() ||
                    prefix == "xmlns" || (prefix == "xml") != (value == xml_namespace) ||
                    value == xmlns_namespace) {
                    return std::unexpected("an XML namespace declaration is invalid");
                }
                if (prefix != "xml") {
                    node->scope[prefix] = value;
                }
            }
        }
        auto parts = split(node->qname);
        if (!parts) {
            return std::unexpected(parts.error());
        }
        node->prefix = parts->first;
        node->local = parts->second;
        auto ns = resolve(*node, node->prefix);
        if (!ns) {
            return std::unexpected(ns.error());
        }
        node->ns = *ns;
        for (auto& [attribute_name, value] : raw) {
            if (attribute_name == "xmlns" || attribute_name.starts_with("xmlns:")) {
                continue;
            }
            auto attribute_parts = split(attribute_name);
            if (!attribute_parts) {
                return std::unexpected(attribute_parts.error());
            }
            std::string attribute_ns;
            if (!attribute_parts->first.empty()) {
                auto resolved = resolve(*node, attribute_parts->first);
                if (!resolved) {
                    return std::unexpected(resolved.error());
                }
                attribute_ns = *resolved;
            }
            if (std::ranges::any_of(node->attributes, [&](const Attribute& existing) {
                    return existing.ns == attribute_ns && existing.local == attribute_parts->second;
                })) {
                return std::unexpected("an XML element repeats an attribute");
            }
            node->attributes.push_back(Attribute{attribute_name,
                                                 attribute_parts->first,
                                                 attribute_parts->second,
                                                 attribute_ns,
                                                 std::move(value)});
        }
        if (empty) {
            return node;
        }
        for (;;) {
            if (position_ >= input_.size()) {
                return std::unexpected("an XML element is unclosed");
            }
            if (starts("</")) {
                position_ += 2;
                auto closing = name();
                if (!closing || *closing != node->qname) {
                    return std::unexpected("an XML end tag does not match its start tag");
                }
                skip_space();
                if (!starts(">")) {
                    return std::unexpected("an XML end tag is unterminated");
                }
                ++position_;
                return node;
            }
            if (starts("<!--")) {
                if (auto skipped = skip_comment(); !skipped) {
                    return std::unexpected(skipped.error());
                }
                continue;
            }
            if (starts("<![CDATA[")) {
                return std::unexpected("XML CDATA sections are not accepted");
            }
            if (starts("<!")) {
                return std::unexpected("document type declarations are not accepted");
            }
            if (starts("<?")) {
                return std::unexpected("processing instructions are not accepted");
            }
            if (starts("<")) {
                auto child = element(node.get(), depth + 1);
                if (!child) {
                    return std::unexpected(child.error());
                }
                node->children.push_back(std::move(*child));
                continue;
            }
            std::string text;
            while (position_ < input_.size() && input_[position_] != '<') {
                const char character = input_[position_];
                if (character == '&') {
                    if (auto decoded = reference(text); !decoded) {
                        return std::unexpected(decoded.error());
                    }
                    continue;
                }
                if (static_cast<unsigned char>(character) < 0x20 && character != '\t' &&
                    character != '\n') {
                    return std::unexpected("XML text contains a control character");
                }
                text += character;
                ++position_;
            }
            // Text either side of a dropped comment is one value, as canonicalization renders it.
            if (!node->children.empty() && node->children.back()->is_text) {
                node->children.back()->text += text;
            } else {
                auto text_node = std::make_unique<Node>();
                text_node->is_text = true;
                text_node->text = std::move(text);
                text_node->parent = node.get();
                node->children.push_back(std::move(text_node));
            }
        }
    }

    std::string input_;
    std::size_t position_ = 0;
    std::size_t elements_ = 0;
};

std::expected<std::unique_ptr<Node>, Error> parse(std::string_view input) {
    if (input.empty() || input.size() > maximum_document_bytes) {
        return std::unexpected("the XML document is empty or too large");
    }
    return Parser(normalize_line_ends(input)).document();
}

// ---------------------------------------------------------------------------------------------
// Exclusive XML Canonicalization 1.0 without comments (https://www.w3.org/TR/xml-exc-c14n/) of
// one element's subtree, optionally leaving one descendant out (the enveloped-signature
// transform).
// ---------------------------------------------------------------------------------------------

std::string escape_text(std::string_view value) {
    std::string result;
    for (const char character : value) {
        switch (character) {
        case '&':
            result += "&amp;";
            break;
        case '<':
            result += "&lt;";
            break;
        case '>':
            result += "&gt;";
            break;
        case '\r':
            result += "&#xD;";
            break;
        default:
            result += character;
        }
    }
    return result;
}

std::string escape_attribute(std::string_view value) {
    std::string result;
    for (const char character : value) {
        switch (character) {
        case '&':
            result += "&amp;";
            break;
        case '<':
            result += "&lt;";
            break;
        case '"':
            result += "&quot;";
            break;
        case '\t':
            result += "&#x9;";
            break;
        case '\n':
            result += "&#xA;";
            break;
        case '\r':
            result += "&#xD;";
            break;
        default:
            result += character;
        }
    }
    return result;
}

std::set<std::string> prefix_list(std::string_view list) {
    std::set<std::string> result;
    std::size_t position = 0;
    while (position < list.size()) {
        while (position < list.size() && xml_space(list[position])) {
            ++position;
        }
        const std::size_t start = position;
        while (position < list.size() && !xml_space(list[position])) {
            ++position;
        }
        if (position > start) {
            const std::string_view token = list.substr(start, position - start);
            result.insert(token == "#default" ? std::string() : std::string(token));
        }
    }
    return result;
}

class Canonicalizer {
public:
    Canonicalizer(const Node* omit, std::set<std::string> inclusive)
        : omit_(omit), inclusive_(std::move(inclusive)) {}

    std::string run(const Node& apex) {
        element(apex, {});
        return std::move(out_);
    }

private:
    void element(const Node& node, const std::map<std::string, std::string>& rendered) {
        std::set<std::string> wanted{node.prefix};
        for (const Attribute& attribute : node.attributes) {
            if (!attribute.prefix.empty()) {
                wanted.insert(attribute.prefix);
            }
        }
        for (const std::string& prefix : inclusive_) {
            if (node.scope.contains(prefix)) {
                wanted.insert(prefix);
            }
        }
        std::map<std::string, std::string> next = rendered;
        std::vector<std::pair<std::string, std::string>> declarations;
        for (const std::string& prefix : wanted) { // ordered: the default ("") first
            if (prefix == "xml") {
                continue;
            }
            const auto in_scope = node.scope.find(prefix);
            const std::string uri = in_scope == node.scope.end() ? std::string() : in_scope->second;
            const auto previous = rendered.find(prefix);
            if (uri.empty()) {
                if (prefix.empty() && previous != rendered.end() && !previous->second.empty()) {
                    declarations.emplace_back("", "");
                    next[""] = "";
                }
                continue;
            }
            if (previous == rendered.end() || previous->second != uri) {
                declarations.emplace_back(prefix, uri);
                next[prefix] = uri;
            }
        }
        std::vector<const Attribute*> attributes;
        for (const Attribute& attribute : node.attributes) {
            attributes.push_back(&attribute);
        }
        std::ranges::sort(attributes, [](const Attribute* left, const Attribute* right) {
            return std::tie(left->ns, left->local) < std::tie(right->ns, right->local);
        });
        out_ += '<';
        out_ += node.qname;
        for (const auto& [prefix, uri] : declarations) {
            out_ += prefix.empty() ? " xmlns=\"" : " xmlns:" + prefix + "=\"";
            out_ += escape_attribute(uri);
            out_ += '"';
        }
        for (const Attribute* attribute : attributes) {
            out_ += ' ';
            out_ += attribute->qname;
            out_ += "=\"";
            out_ += escape_attribute(attribute->value);
            out_ += '"';
        }
        out_ += '>';
        for (const auto& child : node.children) {
            if (child.get() == omit_) {
                continue;
            }
            if (child->is_text) {
                out_ += escape_text(child->text);
            } else {
                element(*child, next);
            }
        }
        out_ += "</";
        out_ += node.qname;
        out_ += '>';
    }

    const Node* omit_;
    std::set<std::string> inclusive_;
    std::string out_;
};

std::string canonical(const Node& apex, const Node* omit, std::string_view prefixes) {
    return Canonicalizer(omit, prefix_list(prefixes)).run(apex);
}

// ---------------------------------------------------------------------------------------------
// Tree helpers. Every lookup is by namespace URI and local name, and every SAML lookup is of a
// direct child, so nothing is found in a place the signature does not vouch for.
// ---------------------------------------------------------------------------------------------

bool is(const Node& node, std::string_view ns, std::string_view local) {
    return !node.is_text && node.ns == ns && node.local == local;
}

std::vector<const Node*> elements(const Node& node) {
    std::vector<const Node*> result;
    for (const auto& child : node.children) {
        if (!child->is_text) {
            result.push_back(child.get());
        }
    }
    return result;
}

std::vector<const Node*> children(const Node& node, std::string_view ns, std::string_view local) {
    std::vector<const Node*> result;
    for (const auto& child : node.children) {
        if (is(*child, ns, local)) {
            result.push_back(child.get());
        }
    }
    return result;
}

const Node* only_child(const Node& node, std::string_view ns, std::string_view local) {
    const auto found = children(node, ns, local);
    return found.size() == 1 ? found.front() : nullptr;
}

std::optional<std::string> attribute(const Node& node, std::string_view local) {
    for (const Attribute& item : node.attributes) {
        if (item.ns.empty() && item.local == local) {
            return item.value;
        }
    }
    return std::nullopt;
}

std::string attribute_or_empty(const Node& node, std::string_view local) {
    return attribute(node, local).value_or(std::string());
}

// The text of an element that holds only text.
std::optional<std::string> text(const Node& node) {
    std::string result;
    for (const auto& child : node.children) {
        if (!child->is_text) {
            return std::nullopt;
        }
        result += child->text;
    }
    return result;
}

bool blank(std::string_view value) {
    return std::ranges::all_of(value, [](char character) { return xml_space(character); });
}

// A structural element (in a Signature) holds elements and white space only.
bool structural(const Node& node) {
    return std::ranges::all_of(
        node.children, [](const auto& child) { return !child->is_text || blank(child->text); });
}

void walk(const Node& node, const auto& visit) {
    if (node.is_text) {
        return;
    }
    visit(node);
    for (const auto& child : node.children) {
        walk(*child, visit);
    }
}

std::size_t count(const Node& root, std::string_view ns, std::string_view local) {
    std::size_t total = 0;
    walk(root, [&](const Node& node) { total += is(node, ns, local) ? 1 : 0; });
    return total;
}

// How many elements carry `value` as an identifier, under any of the names XML Signature
// resolvers accept (ID, Id, id, xml:id). A reference resolves only to an identifier used once.
std::size_t identifier_uses(const Node& root, std::string_view value) {
    std::size_t total = 0;
    walk(root, [&](const Node& node) {
        for (const Attribute& item : node.attributes) {
            const bool identifier = (item.ns.empty() && (item.local == "ID" || item.local == "Id" ||
                                                         item.local == "id")) ||
                                    (item.ns == xml_namespace && item.local == "id");
            if (identifier && item.value == value) {
                ++total;
            }
        }
    });
    return total;
}

// ---------------------------------------------------------------------------------------------
// Encodings and time.
// ---------------------------------------------------------------------------------------------

std::expected<std::string, Error> base64_decode(std::string_view value) {
    std::string compact;
    for (const char character : value) {
        if (!xml_space(character) && character != '\r') {
            compact += character;
        }
    }
    if (compact.empty() || compact.size() > static_cast<std::size_t>(INT_MAX) ||
        compact.size() % 4 != 0) {
        return std::unexpected("a base64 value is invalid");
    }
    std::string decoded((compact.size() / 4) * 3, '\0');
    const int size = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(decoded.data()),
                                     reinterpret_cast<const unsigned char*>(compact.data()),
                                     static_cast<int>(compact.size()));
    if (size < 0) {
        return std::unexpected("a base64 value is invalid");
    }
    const std::size_t padding = compact.ends_with("==") ? 2 : compact.ends_with('=') ? 1 : 0;
    decoded.resize(static_cast<std::size_t>(size) - padding);
    return decoded;
}

std::string base64_encode(std::string_view value) {
    std::string encoded(((value.size() + 2) / 3) * 4, '\0');
    const int size = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
                                     reinterpret_cast<const unsigned char*>(value.data()),
                                     static_cast<int>(value.size()));
    encoded.resize(size < 0 ? 0 : static_cast<std::size_t>(size));
    return encoded;
}

std::string url_encode(std::string_view value) {
    std::string encoded;
    for (const unsigned char byte : value) {
        if (std::isalnum(byte) != 0 || byte == '-' || byte == '.' || byte == '_' || byte == '~') {
            encoded += static_cast<char>(byte);
        } else {
            encoded += std::format("%{:02X}", static_cast<unsigned>(byte));
        }
    }
    return encoded;
}

std::int64_t days_from_civil(std::int64_t year, unsigned month, unsigned day) {
    year -= month <= 2 ? 1 : 0;
    const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
    const auto year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned day_of_era =
        year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + static_cast<std::int64_t>(day_of_era) - 719468;
}

// An xs:dateTime with a time zone ("Z" or ±hh:mm), as Unix seconds; fractions are dropped.
std::optional<std::int64_t> parse_time(std::string_view value) {
    const auto digits = [&](std::size_t at, std::size_t count) -> std::optional<int> {
        if (at + count > value.size()) {
            return std::nullopt;
        }
        int result = 0;
        for (std::size_t index = at; index < at + count; ++index) {
            if (std::isdigit(static_cast<unsigned char>(value[index])) == 0) {
                return std::nullopt;
            }
            result = result * 10 + (value[index] - '0');
        }
        return result;
    };
    if (value.size() < 20 || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
        value[13] != ':' || value[16] != ':') {
        return std::nullopt;
    }
    const auto year = digits(0, 4);
    const auto month = digits(5, 2);
    const auto day = digits(8, 2);
    const auto hour = digits(11, 2);
    const auto minute = digits(14, 2);
    const auto second = digits(17, 2);
    if (!year || !month || !day || !hour || !minute || !second || *month < 1 || *month > 12 ||
        *day < 1 || *day > 31 || *hour > 23 || *minute > 59 || *second > 60) {
        return std::nullopt;
    }
    std::size_t position = 19;
    if (position < value.size() && value[position] == '.') {
        ++position;
        const std::size_t start = position;
        while (position < value.size() &&
               std::isdigit(static_cast<unsigned char>(value[position])) != 0) {
            ++position;
        }
        if (position == start) {
            return std::nullopt;
        }
    }
    std::int64_t offset = 0;
    const std::string_view zone = value.substr(position);
    if (zone == "Z") {
        offset = 0;
    } else if (zone.size() == 6 && (zone[0] == '+' || zone[0] == '-') && zone[3] == ':') {
        const auto zone_hour = digits(position + 1, 2);
        const auto zone_minute = digits(position + 4, 2);
        if (!zone_hour || !zone_minute || *zone_hour > 14 || *zone_minute > 59) {
            return std::nullopt;
        }
        offset = (zone[0] == '+' ? 1 : -1) * (*zone_hour * 3600 + *zone_minute * 60);
    } else {
        return std::nullopt;
    }
    return days_from_civil(*year, static_cast<unsigned>(*month), static_cast<unsigned>(*day)) *
               86400 +
           *hour * 3600 + *minute * 60 + *second - offset;
}

std::string format_time(std::int64_t seconds) {
    std::int64_t days = seconds / 86400;
    std::int64_t rest = seconds % 86400;
    if (rest < 0) {
        rest += 86400;
        --days;
    }
    // civil_from_days
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const auto day_of_era = static_cast<unsigned>(days - era * 146097);
    const unsigned year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    const std::int64_t year = static_cast<std::int64_t>(year_of_era) + era * 400;
    const unsigned day_of_year =
        day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const unsigned month_index = (5 * day_of_year + 2) / 153;
    const unsigned day = day_of_year - (153 * month_index + 2) / 5 + 1;
    const unsigned month = month_index < 10 ? month_index + 3 : month_index - 9;
    return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z",
                       year + (month <= 2 ? 1 : 0),
                       month,
                       day,
                       rest / 3600,
                       (rest / 60) % 60,
                       rest % 60);
}

// Raw DEFLATE (RFC 1951) in stored blocks: valid input for every inflater, no codec needed.
std::string deflate_stored(std::string_view value) {
    std::string out;
    std::size_t position = 0;
    do {
        const std::size_t size = std::min<std::size_t>(value.size() - position, 65535);
        const bool last = position + size == value.size();
        out += static_cast<char>(last ? 1 : 0);
        out += static_cast<char>(size & 0xFF);
        out += static_cast<char>((size >> 8) & 0xFF);
        out += static_cast<char>(~size & 0xFF);
        out += static_cast<char>((~size >> 8) & 0xFF);
        out.append(value.substr(position, size));
        position += size;
    } while (position < value.size());
    return out;
}

std::string escape_xml_attribute(std::string_view value) {
    std::string result;
    for (const char character : value) {
        switch (character) {
        case '&':
            result += "&amp;";
            break;
        case '<':
            result += "&lt;";
            break;
        case '>':
            result += "&gt;";
            break;
        case '"':
            result += "&quot;";
            break;
        default:
            result += character;
        }
    }
    return result;
}

bool https_or_loopback(std::string_view url) {
    return url.starts_with("https://") || url.starts_with("http://127.0.0.1:") ||
           url.starts_with("http://localhost:") || url.starts_with("http://[::1]:");
}

// ---------------------------------------------------------------------------------------------
// XML Signature verification (https://www.w3.org/TR/xmldsig-core1/), restricted to what SAML
// identity providers send: one Reference to the signed element's own ID, the enveloped-signature
// and exclusive-canonicalization transforms, SHA-256 or SHA-512 digests, and RSA PKCS#1 v1.5
// signatures over SHA-256 or SHA-512.
// ---------------------------------------------------------------------------------------------

struct KeyDeleter {
    void operator()(EVP_PKEY* key) const {
        EVP_PKEY_free(key);
    }
};
using Key = std::unique_ptr<EVP_PKEY, KeyDeleter>;

std::expected<Key, Error> certificate_key(std::string_view certificate) {
    std::string pem(certificate);
    if (pem.find("-----BEGIN CERTIFICATE-----") == std::string::npos) {
        // A bare base64 DER certificate, as metadata's X509Certificate carries it.
        std::string compact;
        for (const char character : pem) {
            if (std::isspace(static_cast<unsigned char>(character)) == 0) {
                compact += character;
            }
        }
        pem = "-----BEGIN CERTIFICATE-----\n";
        for (std::size_t position = 0; position < compact.size(); position += 64) {
            pem += compact.substr(position, 64) + "\n";
        }
        pem += "-----END CERTIFICATE-----\n";
    }
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    X509* x509 = bio == nullptr ? nullptr : PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    Key key(x509 == nullptr ? nullptr : X509_get_pubkey(x509));
    X509_free(x509);
    BIO_free(bio);
    if (!key) {
        return std::unexpected("the provider's signing certificate cannot be read");
    }
    if (EVP_PKEY_get_base_id(key.get()) != EVP_PKEY_RSA || EVP_PKEY_get_bits(key.get()) < 2048) {
        return std::unexpected("the provider's signing certificate must hold an RSA key of at "
                               "least 2048 bits");
    }
    return key;
}

// The PrefixList of a CanonicalizationMethod or Transform that names exclusive
// canonicalization: its only allowed child is one InclusiveNamespaces.
std::expected<std::string, Error> exclusive_prefixes(const Node& method) {
    const auto inner = elements(method);
    if (!structural(method) || inner.size() > 1 ||
        (inner.size() == 1 && !is(*inner.front(), exclusive_c14n, "InclusiveNamespaces"))) {
        return std::unexpected("the SAML signature's canonicalization is not understood");
    }
    return inner.empty() ? std::string() : attribute_or_empty(*inner.front(), "PrefixList");
}

std::expected<void, Error> verify_signature(const Node& root,
                                            const Node& signed_element,
                                            const Node& signature,
                                            EVP_PKEY* key) {
    const auto parts = elements(signature);
    if (!structural(signature) || parts.size() < 2 ||
        !is(*parts[0], dsig_namespace, "SignedInfo") ||
        !is(*parts[1], dsig_namespace, "SignatureValue") ||
        (parts.size() > 2 && (parts.size() > 3 || !is(*parts[2], dsig_namespace, "KeyInfo")))) {
        return std::unexpected("the SAML signature's structure is not understood");
    }
    const Node& signed_info = *parts[0];
    const auto info = elements(signed_info);
    if (!structural(signed_info) || info.size() != 3 ||
        !is(*info[0], dsig_namespace, "CanonicalizationMethod") ||
        !is(*info[1], dsig_namespace, "SignatureMethod") ||
        !is(*info[2], dsig_namespace, "Reference")) {
        return std::unexpected("the SAML signature must carry exactly one reference");
    }
    if (attribute_or_empty(*info[0], "Algorithm") != exclusive_c14n) {
        return std::unexpected("the SAML signature must use exclusive canonicalization");
    }
    auto info_prefixes = exclusive_prefixes(*info[0]);
    if (!info_prefixes) {
        return std::unexpected(info_prefixes.error());
    }
    const std::string signature_method = attribute_or_empty(*info[1], "Algorithm");
    const EVP_MD* signature_digest =
        signature_method == "http://www.w3.org/2001/04/xmldsig-more#rsa-sha256"   ? EVP_sha256()
        : signature_method == "http://www.w3.org/2001/04/xmldsig-more#rsa-sha512" ? EVP_sha512()
                                                                                  : nullptr;
    if (signature_digest == nullptr || !elements(*info[1]).empty()) {
        return std::unexpected("the SAML signature algorithm is not accepted (RSA-SHA256 or "
                               "RSA-SHA512)");
    }
    const Node& reference = *info[2];
    const auto identifier = attribute(signed_element, "ID");
    if (!identifier || identifier->empty() ||
        attribute_or_empty(reference, "URI") != "#" + *identifier ||
        identifier_uses(root, *identifier) != 1) {
        return std::unexpected("the SAML signature does not reference the element it signs");
    }
    const auto reference_parts = elements(reference);
    if (!structural(reference) || reference_parts.size() != 3 ||
        !is(*reference_parts[0], dsig_namespace, "Transforms") ||
        !is(*reference_parts[1], dsig_namespace, "DigestMethod") ||
        !is(*reference_parts[2], dsig_namespace, "DigestValue")) {
        return std::unexpected("the SAML signature's reference is not understood");
    }
    const auto transforms = elements(*reference_parts[0]);
    if (!structural(*reference_parts[0]) || transforms.size() != 2 ||
        !is(*transforms[0], dsig_namespace, "Transform") ||
        !is(*transforms[1], dsig_namespace, "Transform") ||
        attribute_or_empty(*transforms[0], "Algorithm") != enveloped_signature ||
        !elements(*transforms[0]).empty() || !structural(*transforms[0]) ||
        attribute_or_empty(*transforms[1], "Algorithm") != exclusive_c14n) {
        return std::unexpected("the SAML signature's transforms must be enveloped-signature then "
                               "exclusive canonicalization");
    }
    auto reference_prefixes = exclusive_prefixes(*transforms[1]);
    if (!reference_prefixes) {
        return std::unexpected(reference_prefixes.error());
    }
    const std::string digest_method = attribute_or_empty(*reference_parts[1], "Algorithm");
    const EVP_MD* reference_digest =
        digest_method == "http://www.w3.org/2001/04/xmlenc#sha256"   ? EVP_sha256()
        : digest_method == "http://www.w3.org/2001/04/xmlenc#sha512" ? EVP_sha512()
                                                                     : nullptr;
    if (reference_digest == nullptr || !elements(*reference_parts[1]).empty()) {
        return std::unexpected("the SAML digest algorithm is not accepted (SHA-256 or SHA-512)");
    }
    const auto digest_text = text(*reference_parts[2]);
    const auto signature_text = text(*parts[1]);
    if (!digest_text || !signature_text) {
        return std::unexpected("the SAML signature's values are not understood");
    }
    auto provided_digest = base64_decode(*digest_text);
    auto signature_value = base64_decode(*signature_text);
    if (!provided_digest || !signature_value) {
        return std::unexpected("the SAML signature's values are not base64");
    }
    const std::string signed_bytes = canonical(signed_element, &signature, *reference_prefixes);
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_size = 0;
    if (EVP_Digest(signed_bytes.data(),
                   signed_bytes.size(),
                   digest,
                   &digest_size,
                   reference_digest,
                   nullptr) != 1 ||
        provided_digest->size() != digest_size ||
        CRYPTO_memcmp(provided_digest->data(), digest, digest_size) != 0) {
        return std::unexpected("the SAML digest does not match the signed element");
    }
    const std::string info_bytes = canonical(signed_info, nullptr, *info_prefixes);
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    const bool verified =
        context != nullptr &&
        EVP_DigestVerifyInit(context, nullptr, signature_digest, nullptr, key) == 1 &&
        EVP_DigestVerifyUpdate(context, info_bytes.data(), info_bytes.size()) == 1 &&
        EVP_DigestVerifyFinal(context,
                              reinterpret_cast<const unsigned char*>(signature_value->data()),
                              signature_value->size()) == 1;
    EVP_MD_CTX_free(context);
    if (!verified) {
        return std::unexpected("the SAML signature is invalid");
    }
    return {};
}

// ---------------------------------------------------------------------------------------------
// The assertion's conditions (SAML core §2.5, Web Browser SSO profile §4.1.4.3).
// ---------------------------------------------------------------------------------------------

struct Window {
    std::int64_t now;
    std::int64_t skew;
};

std::expected<std::optional<std::int64_t>, Error> optional_time(const Node& node,
                                                                std::string_view name) {
    const auto value = attribute(node, name);
    if (!value) {
        return std::optional<std::int64_t>();
    }
    const auto parsed = parse_time(*value);
    if (!parsed) {
        return std::unexpected("a SAML " + std::string(name) + " is not a date and time");
    }
    return parsed;
}

std::expected<std::int64_t, Error>
check_conditions(const Node& assertion, std::string_view audience, Window window) {
    const auto conditions = children(assertion, assertion_namespace, "Conditions");
    if (conditions.size() != 1) {
        return std::unexpected("the SAML assertion must carry one Conditions element");
    }
    const Node& condition = *conditions.front();
    auto not_before = optional_time(condition, "NotBefore");
    auto not_on_or_after = optional_time(condition, "NotOnOrAfter");
    if (!not_before || !not_on_or_after) {
        return std::unexpected(!not_before ? not_before.error() : not_on_or_after.error());
    }
    if (*not_before && window.now + window.skew < **not_before) {
        return std::unexpected("the SAML assertion is not valid yet");
    }
    if (*not_on_or_after && window.now - window.skew >= **not_on_or_after) {
        return std::unexpected("the SAML assertion has expired");
    }
    bool restricted = false;
    for (const Node* item : elements(condition)) {
        if (is(*item, assertion_namespace, "AudienceRestriction")) {
            restricted = true;
            const auto audiences = children(*item, assertion_namespace, "Audience");
            if (!std::ranges::any_of(audiences, [&](const Node* entry) {
                    return text(*entry).value_or(std::string()) == audience;
                })) {
                return std::unexpected("the SAML assertion is for another audience");
            }
        } else if (!is(*item, assertion_namespace, "OneTimeUse") &&
                   !is(*item, assertion_namespace, "ProxyRestriction")) {
            return std::unexpected("the SAML assertion has a condition Lake does not understand");
        }
    }
    if (!restricted) {
        return std::unexpected("the SAML assertion names no audience");
    }
    return not_on_or_after->value_or(INT64_MAX);
}

struct Confirmed {
    std::string in_response_to;
    std::int64_t not_on_or_after;
};

std::expected<Confirmed, Error> check_subject_confirmation(const Node& subject,
                                                           std::string_view acs,
                                                           std::string_view response_in_response_to,
                                                           Window window) {
    Error last = "the SAML assertion has no bearer subject confirmation";
    for (const Node* confirmation : children(subject, assertion_namespace, "SubjectConfirmation")) {
        if (attribute_or_empty(*confirmation, "Method") != bearer_method) {
            continue;
        }
        const auto data = children(*confirmation, assertion_namespace, "SubjectConfirmationData");
        if (data.size() != 1) {
            last = "the SAML bearer confirmation must carry one SubjectConfirmationData";
            continue;
        }
        const Node& item = *data.front();
        auto not_before = optional_time(item, "NotBefore");
        auto not_on_or_after = optional_time(item, "NotOnOrAfter");
        const std::string in_response_to = attribute_or_empty(item, "InResponseTo");
        if (!not_before || !not_on_or_after) {
            last = !not_before ? not_before.error() : not_on_or_after.error();
        } else if (attribute_or_empty(item, "Recipient") != acs) {
            last = "the SAML assertion is for another recipient";
        } else if (!*not_on_or_after) {
            last = "the SAML bearer confirmation has no NotOnOrAfter";
        } else if (window.now - window.skew >= **not_on_or_after) {
            last = "the SAML assertion has expired";
        } else if (*not_before && window.now + window.skew < **not_before) {
            last = "the SAML assertion is not valid yet";
        } else if (in_response_to.empty()) {
            last = "the SAML assertion answers no request: unsolicited responses are not accepted";
        } else if (!response_in_response_to.empty() && response_in_response_to != in_response_to) {
            last = "the SAML response and its assertion answer different requests";
        } else {
            return Confirmed{in_response_to, **not_on_or_after};
        }
    }
    return std::unexpected(last);
}

} // namespace

std::string sp_entity_id(std::string_view public_host, std::string_view provider_id) {
    return "https://" + std::string(public_host) + "/v1/saml/" + url_encode(provider_id) +
           "/metadata";
}

std::string acs_url(std::string_view public_host, std::string_view provider_id) {
    return "https://" + std::string(public_host) + "/v1/saml/" + url_encode(provider_id) + "/acs";
}

std::string start_url(std::string_view public_host, std::string_view provider_id) {
    return "https://" + std::string(public_host) + "/v1/saml/" + url_encode(provider_id) + "/start";
}

std::string sp_metadata(std::string_view public_host, std::string_view provider_id) {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<md:EntityDescriptor xmlns:md=\"" +
           std::string(metadata_namespace) + "\" entityID=\"" +
           escape_xml_attribute(sp_entity_id(public_host, provider_id)) +
           "\">\n"
           "  <md:SPSSODescriptor AuthnRequestsSigned=\"false\" WantAssertionsSigned=\"true\" "
           "protocolSupportEnumeration=\"" +
           std::string(protocol_namespace) +
           "\">\n"
           "    <md:AssertionConsumerService Binding=\"" +
           std::string(post_binding) + "\" Location=\"" +
           escape_xml_attribute(acs_url(public_host, provider_id)) +
           "\" index=\"0\" isDefault=\"true\"/>\n"
           "  </md:SPSSODescriptor>\n"
           "</md:EntityDescriptor>\n";
}

std::expected<std::string, std::string> idp_sso_url(std::string_view metadata,
                                                    std::string_view idp_entity_id) {
    auto document = parse(metadata);
    if (!document) {
        return std::unexpected("the provider's SAML metadata cannot be read: " + document.error());
    }
    const Node* entity = nullptr;
    if (is(**document, metadata_namespace, "EntityDescriptor")) {
        entity = document->get();
    } else if (is(**document, metadata_namespace, "EntitiesDescriptor")) {
        for (const Node* candidate : children(**document, metadata_namespace, "EntityDescriptor")) {
            if (attribute_or_empty(*candidate, "entityID") == idp_entity_id) {
                entity = candidate;
            }
        }
    }
    if (entity == nullptr || attribute_or_empty(*entity, "entityID") != idp_entity_id) {
        return std::unexpected("the provider's SAML metadata does not describe its issuer");
    }
    const Node* descriptor = only_child(*entity, metadata_namespace, "IDPSSODescriptor");
    if (descriptor == nullptr) {
        return std::unexpected("the provider's SAML metadata has no IDPSSODescriptor");
    }
    for (const Node* service : children(*descriptor, metadata_namespace, "SingleSignOnService")) {
        if (attribute_or_empty(*service, "Binding") == redirect_binding) {
            const std::string location = attribute_or_empty(*service, "Location");
            if (!https_or_loopback(location)) {
                return std::unexpected("the provider's SAML sign-in URL must use HTTPS");
            }
            return location;
        }
    }
    return std::unexpected("the provider's SAML metadata has no HTTP-Redirect SingleSignOnService");
}

std::string new_request_id() {
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        return {};
    }
    std::string id = "_";
    for (const unsigned char byte : bytes) {
        id += std::format("{:02x}", static_cast<unsigned>(byte));
    }
    return id;
}

std::string authn_request_url(std::string_view idp_sso_url,
                              std::string_view request_id,
                              std::string_view sp_entity_id,
                              std::string_view acs_url,
                              std::int64_t now) {
    const std::string request =
        "<samlp:AuthnRequest xmlns:samlp=\"" + std::string(protocol_namespace) +
        "\" xmlns:saml=\"" + std::string(assertion_namespace) + "\" ID=\"" +
        escape_xml_attribute(request_id) + "\" Version=\"2.0\" IssueInstant=\"" + format_time(now) +
        "\" Destination=\"" + escape_xml_attribute(idp_sso_url) +
        "\" AssertionConsumerServiceURL=\"" + escape_xml_attribute(acs_url) +
        "\" ProtocolBinding=\"" + std::string(post_binding) + "\"><saml:Issuer>" +
        escape_text(sp_entity_id) +
        "</saml:Issuer><samlp:NameIDPolicy AllowCreate=\"true\"/></samlp:AuthnRequest>";
    return std::string(idp_sso_url) +
           (idp_sso_url.find('?') == std::string_view::npos ? "?" : "&") +
           "SAMLRequest=" + url_encode(base64_encode(deflate_stored(request)));
}

std::expected<std::string, std::string>
canonicalize_exclusive(std::string_view xml, std::string_view inclusive_prefixes) {
    auto document = parse(xml);
    if (!document) {
        return std::unexpected(document.error());
    }
    return canonical(**document, nullptr, inclusive_prefixes);
}

std::expected<std::string, std::string> canonicalize_element(std::string_view xml,
                                                             std::string_view local_name,
                                                             std::string_view omit_local_name,
                                                             std::string_view inclusive_prefixes) {
    auto document = parse(xml);
    if (!document) {
        return std::unexpected(document.error());
    }
    const Node* found = nullptr;
    walk(**document, [&](const Node& node) {
        if (found == nullptr && node.local == local_name) {
            found = &node;
        }
    });
    if (found == nullptr) {
        return std::unexpected("the element is missing");
    }
    const Node* omit = nullptr;
    for (const Node* child : elements(*found)) {
        if (!omit_local_name.empty() && child->local == omit_local_name) {
            omit = child;
        }
    }
    return canonical(*found, omit, inclusive_prefixes);
}

std::expected<Assertion, std::string> verify_response(std::string_view response_base64,
                                                      const Expectations& expected) {
    if (response_base64.size() > maximum_document_bytes * 2) {
        return std::unexpected("the SAML response is too large");
    }
    auto decoded = base64_decode(response_base64);
    if (!decoded) {
        return std::unexpected("the SAML response is not base64");
    }
    auto document = parse(*decoded);
    if (!document) {
        return std::unexpected("the SAML response is not accepted XML: " + document.error());
    }
    const Node& response = **document;
    if (!is(response, protocol_namespace, "Response") ||
        attribute_or_empty(response, "Version") != "2.0" ||
        attribute_or_empty(response, "ID").empty()) {
        return std::unexpected("the document is not a SAML 2.0 Response");
    }
    if (const auto destination = attribute(response, "Destination");
        destination && *destination != expected.acs_url) {
        return std::unexpected("the SAML response is addressed to another destination");
    }
    const Node* status = only_child(response, protocol_namespace, "Status");
    const Node* status_code =
        status == nullptr ? nullptr : only_child(*status, protocol_namespace, "StatusCode");
    if (status_code == nullptr) {
        return std::unexpected("the SAML response has no status");
    }
    if (const std::string value = attribute_or_empty(*status_code, "Value");
        value != status_success) {
        return std::unexpected("the identity provider did not sign the person in (" + value + ")");
    }
    const auto response_issuers = children(response, assertion_namespace, "Issuer");
    if (response_issuers.size() > 1 ||
        (response_issuers.size() == 1 &&
         (text(*response_issuers.front()).value_or(std::string()) != expected.idp_entity_id ||
          attribute(*response_issuers.front(), "Format").value_or(std::string(entity_format)) !=
              entity_format))) {
        return std::unexpected("the SAML response's issuer is not accepted");
    }
    if (count(response, assertion_namespace, "EncryptedAssertion") != 0) {
        return std::unexpected("encrypted SAML assertions are not supported");
    }
    const auto assertions = children(response, assertion_namespace, "Assertion");
    if (assertions.size() != 1 || count(response, assertion_namespace, "Assertion") != 1) {
        return std::unexpected("the SAML response must carry exactly one assertion");
    }
    const Node& assertion = *assertions.front();
    // Only the two places a signature belongs: the response's and the assertion's own.
    const auto response_signatures = children(response, dsig_namespace, "Signature");
    const auto assertion_signatures = children(assertion, dsig_namespace, "Signature");
    if (response_signatures.size() > 1 || assertion_signatures.size() > 1 ||
        count(response, dsig_namespace, "Signature") !=
            response_signatures.size() + assertion_signatures.size()) {
        return std::unexpected("the SAML response carries a signature where none belongs");
    }
    if (response_signatures.empty() && assertion_signatures.empty()) {
        return std::unexpected("the SAML response is not signed");
    }
    auto key = certificate_key(expected.certificate_pem);
    if (!key) {
        return std::unexpected(key.error());
    }
    if (!response_signatures.empty()) {
        if (auto verified =
                verify_signature(response, response, *response_signatures.front(), key->get());
            !verified) {
            return std::unexpected(verified.error());
        }
    }
    if (!assertion_signatures.empty()) {
        if (auto verified =
                verify_signature(response, assertion, *assertion_signatures.front(), key->get());
            !verified) {
            return std::unexpected(verified.error());
        }
    }
    // Everything below is read from the verified assertion, by direct child.
    const auto parts = elements(assertion);
    if (attribute_or_empty(assertion, "Version") != "2.0" ||
        attribute_or_empty(assertion, "ID").empty() || parts.empty() ||
        !is(*parts.front(), assertion_namespace, "Issuer")) {
        return std::unexpected("the SAML assertion is not a SAML 2.0 assertion");
    }
    const auto issuer = text(*parts.front());
    if (!issuer || *issuer != expected.idp_entity_id) {
        return std::unexpected("the SAML assertion's issuer is not accepted");
    }
    const Window window{expected.now, expected.skew_seconds};
    const auto subjects = children(assertion, assertion_namespace, "Subject");
    if (subjects.size() != 1) {
        return std::unexpected("the SAML assertion must carry one subject");
    }
    const Node* name_id = only_child(*subjects.front(), assertion_namespace, "NameID");
    const auto subject = name_id == nullptr ? std::nullopt : text(*name_id);
    if (!subject || blank(*subject)) {
        return std::unexpected("the SAML assertion's subject has no NameID");
    }
    auto confirmed = check_subject_confirmation(
        *subjects.front(), expected.acs_url, attribute_or_empty(response, "InResponseTo"), window);
    if (!confirmed) {
        return std::unexpected(confirmed.error());
    }
    auto conditions_end = check_conditions(assertion, expected.sp_entity_id, window);
    if (!conditions_end) {
        return std::unexpected(conditions_end.error());
    }
    const auto statements = children(assertion, assertion_namespace, "AuthnStatement");
    if (statements.empty()) {
        return std::unexpected("the SAML assertion has no authentication statement");
    }
    for (const Node* statement : statements) {
        auto session_end = optional_time(*statement, "SessionNotOnOrAfter");
        if (!session_end) {
            return std::unexpected(session_end.error());
        }
        if (*session_end && window.now - window.skew >= **session_end) {
            return std::unexpected("the SAML authentication session has ended");
        }
    }
    Assertion result{.id = attribute_or_empty(assertion, "ID"),
                     .in_response_to = confirmed->in_response_to,
                     .expires_at = std::min(confirmed->not_on_or_after, *conditions_end),
                     .issuer = *issuer,
                     .subject = *subject,
                     .attributes = nlohmann::json::object()};
    for (const Node* statement : children(assertion, assertion_namespace, "AttributeStatement")) {
        for (const Node* item : children(*statement, assertion_namespace, "Attribute")) {
            const std::string name = attribute_or_empty(*item, "Name");
            const auto values = children(*item, assertion_namespace, "AttributeValue");
            const auto value = values.empty() ? std::nullopt : text(*values.front());
            if (!name.empty() && value && !result.attributes.contains(name)) {
                result.attributes[name] = *value;
            }
        }
    }
    return result;
}

} // namespace lake::control::saml
