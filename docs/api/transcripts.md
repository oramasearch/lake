# Transcripts and Speech-to-Text

**Status: implemented 2026-10-10 in the source tree (lake#561); not deployed.** A person adds what
was said in a meeting or a voice memo to a workspace, often from a phone: they record, upload an
audio file, upload a `.txt`, `.vtt` or `.srt` file, or paste text. Lake keeps the text in the
workspace Zetta, where it is searchable, and Ask Orama and agents read it. Audio is turned into text
by OpenAI with lake's own key, and **the audio is never kept**.

## Capabilities

| Capability | Kind | Input | Output |
|---|---|---|---|
| `transcribe` | action, any signed-in caller | `{name, audio_base64, language?}` | `{text, seconds?, model}` |
| `create_transcript` | action, `write` on the space | `{customer_id, workspace_id, title, text?, audio_base64?, audio_name?, language?, source, recorded_at?}` | the transcript |
| `list_transcripts` | read, `read` on the space | `{customer_id, workspace_id, q?, limit?, cursor?}` | `{transcripts: [{id, title, excerpt, source, seconds, created_at, created_by, created_by_name}], total, next_cursor?}` |
| `get_transcript` | read, `read` on the space | `{customer_id, workspace_id, id}` | the transcript |
| `update_transcript` | action, `write` on the space | `{customer_id, workspace_id, id, title?, text?}` | the transcript |
| `delete_transcript` | action, `write` on the space | `{customer_id, workspace_id, id}` | `{deleted: true}` |

A transcript is `{id, title, text, source, seconds, language, recorded_at, created_at, created_by,
created_by_name}`:

- `source` is `recording`, `upload` or `text`, as the person chose.
- `seconds` is the audio's length when the speech-to-text model reports it, otherwise `null`.
  `gpt-4o-transcribe` reports usage in tokens, so its transcripts have `null`; `whisper-1` reports
  seconds. Either is the workspace's usage (lake#564): the seconds as `speech`, the tokens as
  `model_tokens` of that model, counted off the request path when the audio is heard
  ([metering](metering.md#dimensions)). Dictation through `transcribe` names no workspace and is not
  counted ([lake#613](https://github.com/oramasearch/lake/issues/613)).
- `language` is the ISO-639-1 code the person gave, otherwise `null`.
- `recorded_at` is when it was said, as the person gave it (an RFC 3339 date or instant), otherwise
  `null`.
- `created_by` is the user id of the person who added it, or the operator's identity.
- `created_by_name` is that person's display name, else their email, read from the control store
  where identity lives (lake#579); `null` for an author lake has no user for, such as an operator
  credential. A page shows it, or "you" for the person viewing, and never the id.
- `created_at` is when lake stored it.

`create_transcript` needs `text` or `audio_base64`:

- With `text`, the text is kept, and `audio_base64` is ignored.
- With audio and no text, lake transcribes the audio first and keeps the text. `seconds` comes from
  the transcription.

`.vtt` and `.srt` uploads arrive as `text`. Lake recognises captions by their content (a `WEBVTT`
header or a cue timing line such as `00:00:01,000 --> 00:00:04,000`) and keeps only what was said:

- The header, `NOTE`, `STYLE` and `REGION` blocks, cue numbers, cue identifiers, timings and
  inline tags are removed.
- A `<v Name>` voice becomes `Name: `.
- A line the next cue repeats (rolling captions) is kept once.
- Each cue becomes one line.

Plain text, including prose with an arrow in it, is kept as written. The pure function is
`clean_caption_text` in `include/lake/api/transcription.hpp`, and `update_transcript` applies it
too.

`list_transcripts` is newest first:

- Each item carries a one-line `excerpt` of the text (200 bytes, never splitting a character), not
  the text.
- `q` keeps the transcripts whose title or text contains it, ignoring ASCII case.
- `limit` is 1 to 100, default 50.
- `next_cursor` is present when more follow. Pass it as `cursor` for the next page. A cursor that
  names no transcript of the list is `invalid_input` at `/cursor`.

Every transcript capability authorizes through the space (`customer_id`), and `Registry::call`
checks that `workspace_id` is a live workspace of that space. Another space's caller is
`unauthorized` before any workspace is read. A transcript of another workspace of the same space
is `not_found`.

Every transcript capability is MCP tool group **Transcripts** in `llms.txt`. `list_transcripts` and
`get_transcript` are reads, so their MCP tools carry `readOnlyHint: true`. Ask Orama's turn key
carries every read, so Ask Orama reads transcripts. A launched agent's credential carries both reads
too (`control::agent_launch_scopes`, [agent-launches.md](agent-launches.md)).

## Where transcripts live

Each transcript is one sealed record in the collection `lake_transcripts` of its workspace's Zetta
([workspace records](../architecture/workspace-records.md)):

- The lookup is the space (`customer:<id>`), so every reader of the space reads it in that
  workspace and no other workspace sees it.
- The body is `{customer_id, title, text, source, seconds?, language, recorded_at, created_by}`,
  sealed with the workspace's key. The workspace holds only ciphertext and a blind index.
- An edit is a conditional write at the revision read, retried on a conflict. A delete removes the
  record at the revision read.
- Nothing about a transcript is in lake's PostgreSQL. Deleting the workspace deletes its
  transcripts with its Zetta.

`list_transcripts` reads the space's records in the workspace (one `list_records` walk) and filters
and pages in lake. A workspace with thousands of long transcripts will want a search index in the
workspace instead; that is not built.

## The audio is never kept

- `transcribe` and `create_transcript` decode the audio into memory for the one call, send it to
  OpenAI and drop it.
- Lake writes no audio to a record, a file, a log or the audit. The record's body has no audio field
  (the test reads the opened body to check).
- The audit event of `transcribe` names only the model. The audit event of `create_transcript`
  names the transcript and its source, never its text.
- The request log names the route pattern and ids only ([telemetry](../serving/telemetry.md)).

OpenAI keeps what its API terms say it keeps; lake sends no other copy anywhere.

## Speech-to-text

Lake calls `POST https://api.openai.com/v1/audio/transcriptions`:

- The request is `multipart/form-data` with the file, `model`, `response_format: json` and
  `language` when given.
- It goes through the HTTP server's one outbound client (`OutboundHttpClient` in
  `src/api/http_server.cpp`), with a five-minute timeout instead of the client's 30-second default.
- It runs on lake's blocking pool, never on the server's thread.
- The file name keeps the caller's extension. A name without a known extension takes one from the
  `data:` URL's media type (`audio/webm` gives `.webm`, `audio/mp4` gives `.mp4`), else `.webm`.

The request is built and the answer read by pure functions in `src/api/transcription.cpp`
(`openai_transcription_body`, `parse_openai_transcription`).

The transcriber is an injected dependency, `lake::api::Transcriber` on `ApiContext` (from
`ServerOptions::transcriber`, or built from the key):

- Tests inject a fake and never call OpenAI.
- With no key and no injected transcriber, the dependency is empty, and both capabilities answer
  `not_supported`.

| Variable | Meaning |
|---|---|
| `LAKE_OPENAI_API_KEY` | OpenAI API key for speech-to-text. Unset: speech-to-text is off. |
| `LAKE_TRANSCRIBE_MODEL` | Transcription model; default `gpt-4o-transcribe` (`whisper-1` and `gpt-4o-mini-transcribe` also work). |

How the dev Lake gets the key is in [deployment](../deployment/aks.md#speech-to-text-key).

## Limits

| What | Limit | Refusal |
|---|---|---|
| Audio, decoded | 25 MiB (OpenAI's upload limit) | `size_invalid` at `/audio_base64` |
| Audio formats | webm, mp4, m4a, mp3, mpga, mpeg, wav, ogg, oga, flac (OpenAI decides) | `engine_error` from OpenAI |
| Request body | 64 MiB of JSON (base64 audio is 4/3 its size) | `413 size_invalid` |
| Text kept | 400,000 bytes, after cleaning (a record is at most 1 MiB of base64 in Zetta) | `size_invalid` at `/text` (or `/audio_base64` for transcribed audio) |
| Title | 1 to 300 bytes, trimmed | `invalid_input` at `/title` |
| `language` | letters and `-`, at most 16 | `invalid_input` at `/language` |
| `recorded_at` | starts `YYYY-MM-DD`, at most 64 | `invalid_input` at `/recorded_at` |
| OpenAI call | 5 minutes | `engine_error` |

The ingress in front of lake must accept a body of 34 MiB for a 25 MiB recording. The operator's
Lake ingress accepts 64 MiB (`proxy-body-size: 64m`).

The call is synchronous: the caller waits while OpenAI transcribes. Lake allows five minutes, but
the proxies in front of lake on dev close a response sooner: nginx's default `proxy-read-timeout`
is 60 s, and Cloudflare's proxy allows 100 s. A recording that OpenAI takes longer than that to
transcribe answers 504 at the edge, although lake finishes the call. Short phone recordings
transcribe well within it. Long ones need an asynchronous transcription (a task the page follows),
which is not built.

## Errors

| Code | When |
|---|---|
| `not_supported` (501) | No speech-to-text on this lake: "Speech-to-text is not configured on this lake." |
| `engine_error` (422) | OpenAI refused or failed. The message is plain: "OpenAI could not transcribe the audio: <OpenAI's message>", "OpenAI is busy or over its quota; try again shortly (HTTP 429)", or "could not reach OpenAI for speech-to-text: …". A refused key (401/403) says so and never echoes OpenAI's message, which can quote part of the key. |
| `invalid_input` (400) | A file name whose extension is not audio or video (`memo.txt is not audio or video: send .webm, .mp4, …`, at `/name` or `/audio_name`; lake#594), not sent to OpenAI. Not base64, an empty audio, a missing or long title, neither text nor audio (`/text`), no speech heard in the audio, an unknown `source`, a bad `language`, `recorded_at` or `cursor`. |
| `size_invalid` (413) | Audio over 25 MiB, or text over 400,000 bytes. |
| `not_found` (404) | No such transcript in this workspace (`/id`). |
| `unauthorized` (403) | No `read`/`write` on the space, or a scoped key without the capability. |
| `busy` (503) | Lake's blocking pool is full; retry. |
| `workspace_not_ready` | The workspace Zetta is not ready. |

## How to verify

- `ctest --test-dir build -R 'Transcripts|TranscriptsLiveInTheirWorkspace|SchemaConformance|AuditRouting'`
  runs the pure tests (`tests/test_transcripts.cpp`), the capabilities against fake workspace
  Zettas in memory and on PostgreSQL (`tests/test_workspace_records.cpp`), the schema conformance
  with a fake transcriber, and the audit classification.
- On a lake with the key:

  ```sh
  curl -H "Authorization: Bearer $KEY" -H 'Content-Type: application/json' \
    -d "{\"name\":\"memo.m4a\",\"audio_base64\":\"$(base64 -i memo.m4a)\"}" \
    https://lake.oramadev.net/v1/transcribe
  ```

## How to change it

- Another provider is another `Transcriber`, built in the `ApiServer` constructor
  (`src/api/http_server.cpp`) from its own variables. The capabilities do not change.
- A new transcript field is added to `TranscriptCodec` (`src/control/transcripts.cpp`) with a
  default in `decode`, which keeps schema version 1. Then update `transcript_json` and its output
  schema in `src/api/transcript_capabilities.cpp`, and this page.

## Counting Transcripts (lake#574)

`list_transcripts` answers `total`: every transcript `q` keeps in the workspace, on every page, not
only the page's own rows. `limit` is 1 to 100; a larger one is `400 invalid_input` at `/limit`. A
caller that wants only the count asks for `limit: 1` and reads `total`, as Home's Transcripts figure
does. Home asked for `limit: 500` before, which answered 400, and showed "—".

Verify: `GET /v1/list_transcripts?customer_id=<space>&workspace_id=<workspace>&limit=1` answers
`{"transcripts": [<the newest>], "total": <n>, "next_cursor": ...}`.
`WorkspaceRecordStores.TranscriptsLiveInTheirWorkspaceAndKeepNoAudio`
(`tests/test_workspace_records.cpp`) and `Transcripts.PagesFilterIgnoringCaseAndFollowTheCursor`
cover `total` and `created_by_name`.
