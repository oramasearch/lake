#include "lake/control/memory_store.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <format>
#include <random>

#include "lake/control/handles.hpp"

namespace lake::control {

std::string new_uuid_v4() {
    thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<unsigned> byte(0, 255);
    unsigned char bytes[16];
    for (auto& b : bytes) b = static_cast<unsigned char>(byte(rng));
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);  // version 4
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);  // RFC 4122 variant
    std::string out;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out += '-';
        out += std::format("{:02x}", bytes[i]);
    }
    return out;
}

std::string now_iso8601() {
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return std::format("{:%FT%T}Z", now);
}

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

nlohmann::json nullable(const std::string& s) { return s.empty() ? nlohmann::json(nullptr) : nlohmann::json(s); }

}  // namespace

nlohmann::json to_json(const User& u) {
    return {{"id", u.id},           {"taap_id", nullable(u.taap_id)}, {"email", nullable(u.email)},
            {"display_name", u.display_name}, {"created_at", u.created_at}, {"updated_at", u.updated_at}};
}
nlohmann::json to_json(const Organization& o) {
    return {{"id", o.id}, {"taap_id", nullable(o.taap_id)}, {"display_name", o.display_name},
            {"created_at", o.created_at}, {"updated_at", o.updated_at}};
}
nlohmann::json to_json(const Customer& c) {
    return {{"id", c.id},
            {"customer_type", c.customer_type},
            {"user_id", nullable(c.user_id)},
            {"organization_id", nullable(c.organization_id)},
            {"name", c.name},
            {"created_at", c.created_at},
            {"updated_at", c.updated_at}};
}

Result<User> InMemoryStore::create_user(const NewUser& in) {
    std::lock_guard lock(mutex_);
    for (const auto& u : users_) {
        if (!in.taap_id.empty() && u.taap_id == in.taap_id) return std::unexpected(StoreError{"conflict", "taap_id already exists"});
        if (!in.email.empty() && lower(u.email) == lower(in.email)) return std::unexpected(StoreError{"conflict", "email already exists"});
    }
    const std::string now = now_iso8601();
    users_.push_back(User{new_uuid_v4(), in.taap_id, in.email, in.display_name, now, now});
    return users_.back();
}

Result<std::vector<User>> InMemoryStore::list_users(int limit) {
    std::lock_guard lock(mutex_);
    std::vector<User> out(users_.rbegin(), users_.rend());
    if (limit > 0 && out.size() > static_cast<std::size_t>(limit)) out.resize(static_cast<std::size_t>(limit));
    return out;
}

Result<User> InMemoryStore::get_user(const std::string& id) {
    std::lock_guard lock(mutex_);
    for (const auto& u : users_) if (u.id == id) return u;
    return std::unexpected(StoreError{"not_found", "no user " + id});
}

Result<Organization> InMemoryStore::create_organization(const NewOrganization& in) {
    std::lock_guard lock(mutex_);
    for (const auto& o : organizations_) {
        if (!in.taap_id.empty() && o.taap_id == in.taap_id) return std::unexpected(StoreError{"conflict", "taap_id already exists"});
    }
    const std::string now = now_iso8601();
    organizations_.push_back(Organization{new_uuid_v4(), in.taap_id, in.display_name, now, now});
    return organizations_.back();
}

Result<std::vector<Organization>> InMemoryStore::list_organizations(int limit) {
    std::lock_guard lock(mutex_);
    std::vector<Organization> out(organizations_.rbegin(), organizations_.rend());
    if (limit > 0 && out.size() > static_cast<std::size_t>(limit)) out.resize(static_cast<std::size_t>(limit));
    return out;
}

Result<Organization> InMemoryStore::get_organization(const std::string& id) {
    std::lock_guard lock(mutex_);
    for (const auto& o : organizations_) if (o.id == id) return o;
    return std::unexpected(StoreError{"not_found", "no organization " + id});
}

Result<Customer> InMemoryStore::create_customer(const NewCustomer& in) {
    std::lock_guard lock(mutex_);
    const bool individual = in.customer_type == "individual";
    if (!individual && in.customer_type != "organization") {
        return std::unexpected(StoreError{"invalid_input", "customer_type must be individual or organization"});
    }
    if (individual ? (in.user_id.empty() || !in.organization_id.empty()) : (in.organization_id.empty() || !in.user_id.empty())) {
        return std::unexpected(StoreError{"invalid_input", "an individual has exactly user_id, an organization exactly organization_id (customer_type_check)"});
    }
    if (individual && std::none_of(users_.begin(), users_.end(), [&](const User& u) { return u.id == in.user_id; })) {
        return std::unexpected(StoreError{"invalid_input", "user_id does not reference a user"});
    }
    if (!individual && std::none_of(organizations_.begin(), organizations_.end(), [&](const Organization& o) { return o.id == in.organization_id; })) {
        return std::unexpected(StoreError{"invalid_input", "organization_id does not reference an organization"});
    }
    if (!valid_handle(in.name)) return std::unexpected(StoreError{"invalid_input", "name must match customer_name_format"});
    for (const auto& c : customers_) {
        if (c.name == in.name) return std::unexpected(StoreError{"conflict", "name already taken (customer_name_unique)"});
        if (individual && c.user_id == in.user_id) return std::unexpected(StoreError{"conflict", "this user already has a customer"});
        if (!individual && c.organization_id == in.organization_id) return std::unexpected(StoreError{"conflict", "this organization already has a customer"});
    }
    const std::string now = now_iso8601();
    customers_.push_back(Customer{new_uuid_v4(), in.customer_type, in.user_id, in.organization_id, in.name, now, now});
    return customers_.back();
}

Result<std::vector<Customer>> InMemoryStore::list_customers(int limit) {
    std::lock_guard lock(mutex_);
    std::vector<Customer> out(customers_.rbegin(), customers_.rend());
    if (limit > 0 && out.size() > static_cast<std::size_t>(limit)) out.resize(static_cast<std::size_t>(limit));
    return out;
}

Result<Customer> InMemoryStore::get_customer(const std::string& id) {
    std::lock_guard lock(mutex_);
    for (const auto& c : customers_) if (c.id == id) return c;
    return std::unexpected(StoreError{"not_found", "no customer " + id});
}

}  // namespace lake::control
