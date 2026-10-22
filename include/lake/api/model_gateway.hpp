#pragma once
// The model gateway (lake#248, docs/api/model-gateway.md): every model call an agent makes for a
// workspace goes through lake, which forwards it to the model provider with Orama's own key
// (LAKE_OPENAI_API_KEY, a platform environment variable), counts the tokens the provider reports
// into the workspace's `model_tokens` usage, and refuses the call once the account's monthly
// model-token limit is used up. Agents hold no provider key: they point the OpenAI SDK's base URL
// at `<lake>/openai/v1` and use their task, launch or schedule credential as its API key.
//
// Request preparation and usage parsing are pure; model entitlement also reads published prices
// when billing is on. The HTTP exchange is the server's outbound client, injected as a ModelPost,
// and tests inject a fake ModelProvider.

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "lake/api/registry.hpp"

namespace lake::api {

// The OpenAI-compatible endpoints the gateway forwards, as they follow `/openai/v1/`.
inline constexpr std::array<std::string_view, 3> model_gateway_endpoints{
    "chat/completions", "responses", "embeddings"};
bool is_model_gateway_endpoint(std::string_view endpoint);

// OpenAI's API, unless LAKE_MODEL_PROVIDER_URL names another OpenAI-compatible base.
inline constexpr std::string_view default_model_provider_url = "https://api.openai.com/v1";

// Parse LAKE_MODEL_GATEWAY_MODELS: absent uses the defaults; [] disables the gateway.
// Malformed lists, duplicate names and unsupported providers are configuration errors.
std::expected<std::vector<GatewayModel>, std::string> parse_gateway_models(std::string_view text);
bool gateway_model_allowed(const std::vector<GatewayModel>& models, std::string_view name);
// A token billing class binds the exact requested model to its input or output tokens.
std::string model_token_class(std::string_view model, std::string_view kind);
// Reject unpriced model usage before it can enter a billed account's ledger. Billing off: no check.
std::expected<void, ApiError> validate_model_prices(std::string_view model,
                                                    ApiContext& context,
                                                    std::string_view path = "/request/model");
// Check the configured allowlist and, when billing is registered, both published token prices.
std::expected<void, ApiError> validate_gateway_model(std::string_view model, ApiContext& context);

// The request as lake sends it to the provider, or invalid_input naming the field at fault. A
// streamed chat completion is asked to report its usage in its last event
// (`stream_options.include_usage`), so a stream is counted like any other answer. A request with
// `background` set (the Responses API's) is refused: the provider would go on generating after its
// answer, which reports no usage, and the gateway has no route to read the result (lake#939).
std::expected<nlohmann::json, ApiError> prepare_model_request(std::string_view endpoint,
                                                              nlohmann::json request);

// The tokens one answer used, as the provider reported them: `usage.prompt_tokens` and
// `usage.completion_tokens` (chat completions, embeddings) or `usage.input_tokens` and
// `usage.output_tokens` (responses), from the JSON answer or, for a stream, from the event that
// carries them. `reported` is false when the answer carried no usage.
struct ModelUsage {
    std::string model;
    std::uint64_t input_tokens = 0;
    std::uint64_t output_tokens = 0;
    bool reported = false;
};
ModelUsage model_usage(std::string_view body, bool stream);

// The tokens counted for `bytes` bytes of a request or an answer whose usage the provider never
// reported (an answer that failed part way, or one without `usage`, lake#939): one for every four
// bytes, rounded up. That is about what OpenAI's tokenizer makes of English text, and an answer's
// bytes include its JSON and, for a stream, each event's framing, so an answer is counted above the
// text it carried.
std::uint64_t estimated_model_tokens(std::size_t bytes);

// The usage of an event stream read as it arrives (lake#665): feed it each piece of the body in
// order, however the pieces cut the events, and `usage()` is what model_usage would say of the
// whole stream so far. A last line without its line end is read by `finish()`.
class ModelStreamUsage {
public:
    void feed(std::string_view piece);
    void finish();
    const ModelUsage& usage() const {
        return usage_;
    }
    // The bytes fed so far.
    std::uint64_t bytes() const {
        return bytes_;
    }

private:
    void line(std::string_view text);
    std::string partial_;
    ModelUsage usage_;
    std::uint64_t bytes_ = 0;
};

// The provider's refusal as a documented error: its own message for a request it refused
// (engine_error), rate_limited when it is busy, and store_unavailable when it is down or refuses
// Lake's own key (whose message may echo part of the key, so it is never passed on).
ApiError model_provider_failure(int status, std::string_view body, std::string_view transport);

// One HTTPS POST: url, body, headers, timeout, and where a 2xx answer's body goes as it arrives
// (empty: kept whole in the response).
using ModelPost =
    std::function<ModelProviderResponse(const std::string& url,
                                        std::string body,
                                        std::vector<std::pair<std::string, std::string>> headers,
                                        std::chrono::seconds timeout,
                                        ModelBodySink on_body)>;
// A ModelProvider that calls `base_url` (empty: OpenAI's) with `api_key` through `post`.
ModelProvider openai_model_provider(std::string api_key, std::string base_url, ModelPost post);

} // namespace lake::api
