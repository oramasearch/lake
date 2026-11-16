// SPDX-License-Identifier: MIT

#include "repository_people.hpp"

#include <optional>
#include <set>

namespace lake::api {

nlohmann::json signer_people(ApiContext& context, const std::vector<std::string>& taap_ids) {
    nlohmann::json people = nlohmann::json::object();
    if (!context.store || taap_ids.empty()) {
        return people;
    }
    const auto names = context.store->signer_names(taap_ids);
    if (!names) {
        return people;
    }
    for (const auto& [taap_id, name] : *names) {
        people[taap_id] = {
            {"kind", name.kind}, {"name", name.display_name}, {"handle", name.handle}};
    }
    return people;
}

nlohmann::json signer_people_schema() {
    return {{"type", "object"},
            {"description",
             "Who each signer or principal named in this answer is: the TAAP identifier mapped "
             "to the account's kind (user or organization), name and handle. An identity no "
             "account acts for is absent."},
            {"additionalProperties",
             {{"type", "object"},
              {"properties",
               {{"kind", {{"type", "string"}}},
                {"name", {{"type", "string"}}},
                {"handle", {{"type", "string"}}}}}}}};
}

void forge_record_signers(const nlohmann::json& value, std::vector<std::string>& out) {
    if (!value.is_object()) {
        return;
    }
    const auto add = [&out](const nlohmann::json& identity) {
        if (identity.is_string() && !identity.get_ref<const std::string&>().empty()) {
            out.push_back(identity.get<std::string>());
        } else if (identity.is_object() && identity.contains("taap_id")) {
            const nlohmann::json& id = identity.at("taap_id");
            if (id.is_string() && !id.get_ref<const std::string&>().empty()) {
                out.push_back(id.get<std::string>());
            }
        }
    };
    for (const char* field : {"author", "opened_by"}) {
        if (const auto found = value.find(field); found != value.end()) {
            add(*found);
        }
    }
    if (const auto reviewers = value.find("reviewers");
        reviewers != value.end() && reviewers->is_array()) {
        for (const nlohmann::json& reviewer : *reviewers) {
            add(reviewer);
        }
    }
    if (const auto approval = value.find("approval");
        approval != value.end() && approval->is_object() && approval->contains("by")) {
        add(approval->at("by"));
    }
}

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
