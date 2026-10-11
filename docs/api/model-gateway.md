# The model gateway

**Issue:** [lake#248](https://github.com/oramasearch/lake/issues/248), part of the metering epic
[lake#658](https://github.com/oramasearch/lake/issues/658) · **Code:** `include/lake/api/model_gateway.hpp`,
`src/api/model_gateway.cpp`, `model_completion` in `src/api/usage_capabilities.cpp`, the
`/openai/v1/*` routes in `src/api/http_server.cpp` · **Tests:** `tests/test_model_gateway.cpp`

Every model call an agent makes for a workspace goes through Lake. Lake sends it to the model
provider with Orama's own key, counts the tokens the provider reports as the workspace's
`model_tokens`, and refuses the call once the account's monthly `model_tokens` limit is used up.
Agents hold no provider key, so there is no other way for them to reach a model, and nothing
depends on an agent reporting what it spent.

## Why one point in Lake

The alternative was for each agent to report its own tokens (`record_model_usage`). That trusts every
agent, including the code people write for their own agents on agent-runtime, to report every call,
and it leaves Orama's provider key in every agent pod, where a person's agent code can read it. The
gateway counts at the one place every call must pass:

- the caller is identified by the credential Lake already gives each run (below), so the workspace,
  agent and run are Lake's own records, not something the agent says;
- the tokens are the provider's own `usage` field in the answer Lake relays;
- the provider key is a platform environment variable of Lake's (`LAKE_OPENAI_API_KEY`), never in an
  agent pod or a user's secret store.

## Calling it

The gateway speaks the OpenAI API, so an agent uses the OpenAI SDK unchanged, with two settings:

```python
from openai import OpenAI

client = OpenAI(base_url=f"{lake_url}/openai/v1", api_key=lake_token)
client.chat.completions.create(model="gpt-5.5", messages=[...])
```

| Route | Body | Answer |
|---|---|---|
| `POST /openai/v1/chat/completions` | OpenAI's chat completion request | the provider's answer |
| `POST /openai/v1/responses` | OpenAI's Responses request | the provider's answer |
| `POST /openai/v1/embeddings` | OpenAI's embeddings request | the provider's answer |

`lake_url` is Lake's address (an agent pod's `LAKE_URL`). `lake_token` is the run's own credential:

| Credential | Who holds it | Counted in |
|---|---|---|
| task, `lat.<task>.<secret>` | a library agent, for one turn: the `lake_token` in the claimed task's body (Ask Orama, the Project Agent, ApplicationAgent, ExperimentsAgent, Auto-Suggest, AgentCreator, the Eval Generator and the two corpus generators) | the task's workspace, labelled with the agent and the task id |
| launch, `lal.<launch>.<secret>` | an agent launched on agent-runtime (`LAKE_TOKEN`) | the launch's workspace, labelled with the agent's name and the launch id |
| schedule, `las.<schedule>.<secret>` | a scheduled agent's runs | the schedule's workspace, labelled with the agent's name and the schedule id |

Anything else is refused before the provider is called: a person's key or session and the operator
credential are `unauthorized` (403), and so is a library agent's bus credential, which names no turn.
No credential is `unauthenticated` (401). A task credential stops working when its task finishes, and
each call renews the task's lease, as every call with it does.

The path is `/openai/v1/`, not `/v1/`, because `/v1/<name>` is the capability registry's
([README](README.md)). Like the OCI registry at `/v2/` and the Hub routes at `/api/models/`, the
gateway is a protocol route named for its protocol.

### Streaming

`stream: true` is accepted. For a chat completion Lake adds `stream_options.include_usage: true`, so
the provider reports the stream's usage in its last event (the Responses API always does, in
`response.completed`). The answer is `text/event-stream` with the provider's events, unchanged. Lake
sends the stream once the provider has finished it, not event by event; incremental delivery is
[lake#665](https://github.com/oramasearch/lake/issues/665). None of the agents stream today.

### Errors

A refusal is OpenAI's error shape, with Lake's documented code ([errors.md](errors.md)) as `type`
and `code`, and the JSON pointer of the field at fault as `param`:

```json
{"error": {"message": "model: name the model, such as gpt-5.5", "type": "invalid_input",
           "code": "invalid_input", "param": "/request/model"}}
```

| Code | Status | When |
|---|---|---|
| `invalid_input` | 400 | the body is not a JSON object, names no `model`, or streams an embedding |
| `unauthenticated`, `unauthorized` | 401, 403 | no credential, or one that is not a run's (above) |
| `usage_limit_reached` | 429 | the account's monthly `model_tokens` limit is used up ([Limits](#limits)) |
| `engine_error` | 422 | the provider refused the request; the message is the provider's own |
| `rate_limited` | 429 | the provider is busy or over its quota |
| `store_unavailable` | 503 | the provider is down, unreachable, or refused Lake's key (its message, which may echo part of the key, is not passed on) |
| `not_supported` | 501 | this Lake has no provider key (`LAKE_OPENAI_API_KEY`) |

The OpenAI SDK retries 429 and 5xx answers itself.

## The capability: `model_completion`

The routes are a rendering of one registry capability, as `/workspaces/<customer>/zetta/v1/*` is of
`workspace_query` and `workspace_act`. REST is `POST /v1/model_completion`; MCP lists it as a tool.

| Field | Type | Meaning |
|---|---|---|
| `endpoint` (input) | `chat/completions`, `responses` or `embeddings` | the OpenAI API path |
| `request` (input) | object | the OpenAI request body |
| `workspace_id`, `agent`, `launch_id` | string | the run the call was counted for |
| `model` | string | the model the provider says answered (else the one asked for) |
| `input_tokens`, `output_tokens` | integer | the provider's `usage` |
| `usage_reported` | boolean | false when the provider's answer carried no `usage` (counted as zero, and audited so) |
| `recorded` | boolean | the tokens were added to the workspace's usage |
| `streamed` | boolean | the request streamed |
| `response` | object | the provider's answer, when not streamed |
| `events` | string | the provider's whole event stream, when streamed |

It requires no grant (`requires_auth` false, like `record_model_usage`): the handler authorizes it by
the credential's kind and finds the run from it.

## What is counted

Each answered call adds two values to the run's workspace's `lake_usage` counter
([metering.md](metering.md#usage-over-time)): `model_tokens` with `kind: input` and with
`kind: output`, each labelled `agent`, `launch_id` (the task, launch or schedule id) and `model`. Those
are the labels `usage_over_time` breaks `model_tokens` down by (`by_agent`, `by_launch`, `by_model`),
which the Usage dashboard and an agent's page show, and the bill prices `model_tokens` at one rate
([billing.md](billing.md#price-list)). A refused call, by Lake or by the provider, counts nothing.

## Limits

`set_usage_limit {customer_id, dimension: "model_tokens", limit}` is the account's monthly limit, and
the gateway enforces it: once this UTC month's `model_tokens` across the account's workspaces reach
the limit, every model call is refused with `usage_limit_reached` before the provider is called.
`usage_limits` reports the limit as `enforced: true`. The month's use is read from the workspaces at
most once a minute per account and Lake process, and the tokens a process counts in between are added
to it, so a limit can be passed by about a minute of the account's calls on each Lake replica.

## Audit

Each call is one event in the run's workspace audit ([audit.md](audit.md#workspace-audit)), object
`agent_usage/<launch_id>`, action `model_completion`, with `endpoint`, the model asked for and the one
that answered, `streamed`, `provider_status`, `input_tokens`, `output_tokens` and `usage_reported`.
The prompt and the answer are never in the audit.

## Configuration

| Variable | Meaning |
|---|---|
| `LAKE_OPENAI_API_KEY` | Orama's provider key, a platform environment variable of the Lake pod (on the dev cluster, `spec.env` from Secret `lake-prod-openai`). Unset, the gateway answers `not_supported` |
| `LAKE_MODEL_PROVIDER_URL` | an OpenAI-compatible base URL; default `https://api.openai.com/v1` |

Agent pods need neither: they reach the model through Lake. The operator's
`spec.agents.openaiSecretName`, which used to put the key into every agent pod, is left unset once the
agents use the gateway ([deployment/aks.md](../deployment/aks.md#speech-to-text-key)).

## Lake's own model calls

Lake itself calls a model only for speech-to-text (`transcribe`, and `create_transcript` from audio),
which is metered as `speech` by the minute in the workspace it is used in, dictation included
(lake#613), and never also as `model_tokens`: a transcription model's tokens are audit detail
only, so no recording is billed twice ([transcripts.md](transcripts.md#how-long-a-recording-is),
lake#658). Lake makes no other model call (no clean-up or naming of its own).

## `record_model_usage`

`record_model_usage` stays for a model call made outside Lake, such as a model an agent runs itself.
A call through the gateway is already counted and must not be reported again.
