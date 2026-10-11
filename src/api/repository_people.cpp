#include "repository_people.hpp"

#include <optional>
#include <set>

namespace lake::api {

nlohmann::json repository_person_json(const RepositoryPerson& person) {
    return {{"taap_id", person.taap_id},
            {"kind", person.kind},
            {"display_name", person.display_name},
            {"email", person.email},
            {"handle", person.handle}};
}

std::expected<std::vector<RepositoryPerson>, ApiError>
repository_people(ApiContext& context, const control::Repository& repository) {
    const auto failure = [](const control::StoreError& error) {
        return std::unexpected(ApiError{error.code, error.message, {}});
    };
    const auto customer = context.store->get_customer(repository.customer_id);
    if (!customer) {
        return failure(customer.error());
    }
    std::vector<RepositoryPerson> people;
    std::set<std::string> seen;
    const auto add_user = [&](const std::string& user_id) -> std::optional<ApiError> {
        const auto user = context.store->get_user(user_id);
        if (!user) {
            return user.error().code == "not_found"
                       ? std::nullopt
                       : std::optional<ApiError>(
                             ApiError{user.error().code, user.error().message, {}});
        }
        if (user->taap_id.empty() || !seen.insert(user->taap_id).second) {
            return std::nullopt;
        }
        std::string handle;
        if (const auto own = context.store->list_customers(10, user->id); own) {
            for (const control::Customer& space : *own) {
                if (space.customer_type == "individual") {
                    handle = space.name;
                    break;
                }
            }
        }
        people.push_back({user->taap_id, "user", user->display_name, user->email, handle});
        return std::nullopt;
    };
    if (customer->customer_type == "individual") {
        if (auto error = add_user(customer->user_id)) {
            return std::unexpected(*error);
        }
        return people;
    }
    const auto organization = context.store->get_organization(customer->organization_id);
    if (!organization) {
        return failure(organization.error());
    }
    if (!organization->taap_id.empty() && seen.insert(organization->taap_id).second) {
        people.push_back({organization->taap_id,
                          "organization",
                          organization->display_name,
                          "",
                          customer->name});
    }
    const auto members = context.store->list_memberships(customer->organization_id, "", 1000);
    if (!members) {
        return failure(members.error());
    }
    for (const control::Membership& member : *members) {
        if (member.principal_type != "user") {
            continue;
        }
        if (auto error = add_user(member.principal_id)) {
            return std::unexpected(*error);
        }
    }
    return people;
}

} // namespace lake::api
