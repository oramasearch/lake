#pragma once
#include <string>
#include <string_view>

namespace lake::control {

// The customer handle: one global namespace, lowercase letters, digits and inner hyphens, 1–256
// chars (db/schema.sql customer_name_format). Generated as {adjective}-{noun}-{number}.
bool valid_handle(const std::string& name);
std::string generate_handle();
// The rule as a refusal says it, naming the field: "<field> is 1 to 256 lowercase letters, …"
// (lake#592).
std::string handle_rule(std::string_view field);

}  // namespace lake::control
