# Transcripts and Speech-to-Text

**Data → Transcripts** keeps what was said in meetings, voice memos and calls as text you can
search, edit and ask Orama about. A transcript can come from a recording made in the browser, an
uploaded audio file, an uploaded text or caption file, or pasted text. It works well from a phone:
record on the phone, and the transcript is in the workspace a moment later.

## Concepts

- **Transcript.** A title and its text, with where it came from (`source`: `recording`, `upload`
  or `text`), how long the audio was when the speech-to-text model reports it, the language you
  gave, when it was said, who added it and when.
- **Where it lives.** Each transcript is a sealed record in the workspace it was added to. Other workspaces never see it, and deleting the workspace deletes its
  transcripts.
- **Speech-to-text.** Lake turns audio into text with OpenAI's transcription model, using lake's
  own key. **The audio is never kept**: lake reads it into memory for the one call, sends it to
  OpenAI and drops it. No audio is written to a record, a file, a log or the audit.
- **Captions are cleaned.** A `.vtt` or `.srt` file loses its timings, cue numbers, header and
  styling; a `<v Name>` voice becomes `Name: `; a line the next cue repeats is kept once; each cue
  becomes one line. Plain text is kept as written.

## The Transcripts Page

**Data → Transcripts** shows three figures (Transcripts, Recorded, Latest), a search box (*Search
transcripts*) that matches the title or the text, and a card per transcript, newest first: its
title, date, an excerpt, its source, its length and who added it. **Show more** loads the next page.

## Add a Transcript

Press **New transcript**. On a phone it fills the screen, with its save button above the keyboard. Choose
one of four ways:

### Record

1. Choose **Record**.
2. Tap the red microphone and allow the browser to use it.
3. Speak; tap **Stop recording** when done (or Cancel to discard it).
4. Listen back, give it a title (it starts as the time of the recording), and press **Transcribe
   and save**.

While lake transcribes, the sheet shows the time elapsed; keep it open. A recording stops itself
after 60 minutes or just before 25 MB.

### Upload Audio

1. Choose **Upload audio**.
2. Pick a voice memo or a recorded call. On an iPhone, choose **Choose File**, then **Files**
   (where a voice memo saved with **Save to Files** is) or **Browse**.
3. Check the title (it starts as the file name) and press **Transcribe and save**.

**From your phone**, beside the choices, explains how to save a voice memo or a recorded call to
Files on an iPhone or Android so you can pick it here ([Using Lake on a Phone](/docs/phone)).

### Upload Text

1. Choose **Upload text**.
2. Pick a `.txt`, `.md`, `.vtt` or `.srt` file. The browser reads it and shows a **Preview**.
3. Press **Save transcript**. Caption files lose their timings, as above.

### Paste

1. Choose **Paste**.
2. Type, paste or dictate the text (the microphone in the box adds what you say).
3. Give it a title (it starts as the first line) and press **Save transcript**.

On success the new transcript's page opens and the page says *Transcript saved*.

### From the API

`create_transcript` takes `title`, `source` and either `text` or `audio_base64`. With `text`, the
text is kept. With audio and no text, lake transcribes the audio first.

`POST /v1/create_transcript`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "title": "Weekly sync with the design team", "source": "text", "text": "Ana: the renewal list is ready.\nBo: send it to customer success."}
```

To transcribe a recording, send the audio as base64 (a `data:` URL works too) with its file name,
so its extension tells the format:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "title": "Call with Acme", "source": "upload", "audio_base64": "<base64 of memo.m4a>", "audio_name": "memo.m4a", "language": "en", "recorded_at": "2026-10-09T14:05:00Z"}
```

`language` is an ISO-639-1 code such as `en`, and `recorded_at` an RFC 3339 date or instant; both
are optional. The MCP tool is `create_transcript`, with the same JSON.

`customer_id` is the id of the account that owns your workspaces (yours, or your organization's),
and `workspace_id` the workspace ([Getting Started](/docs)).

## Read, Edit, Copy and Delete

Open a transcript from the list. Its page shows the title and text, with figures for words,
length, source and when it was recorded, and four actions:

- **Ask Orama about this** opens Ask Orama (full screen on a phone) with the transcript as its
  context: a chip names the transcript, and your next message is sent with its text (up to 20,000
  characters). Ask it for a summary, the decisions, the action items, or how it relates to your
  data ([Ask Orama](/docs/ask-orama)).
- **Copy** copies the text.
- **Edit** opens **Edit Transcript** with the title and text (and the microphone); **Save** sends
  only what changed.
- **Delete** asks *Delete “<title>”?* and removes the transcript and its text from the workspace.

The same through the API:

`GET /v1/list_transcripts?customer_id=<customer id>&workspace_id=<workspace id>&q=renewal&limit=20`

- `q` keeps the transcripts whose title or text contains it, ignoring case.
- `limit` is 1 to 100 (default 50). When more follow, the answer has `next_cursor`; pass it as
  `cursor`.
- Each item carries a one-line `excerpt`, not the text.

`GET /v1/get_transcript?customer_id=<customer id>&workspace_id=<workspace id>&id=<transcript id>`

`POST /v1/update_transcript`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "id": "<transcript id>", "title": "Weekly sync, October 9"}
```

`POST /v1/delete_transcript`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "id": "<transcript id>"}
```

`update_transcript` cleans caption text the same way as an upload.

## Dictation

Wherever you see the microphone, you can speak instead of typing: in Ask Orama's message box (the
panel and the full page), and in **Paste** and **Edit Transcript**. Tap it, speak, and tap again:
the text is added after what is already in the box, to edit or send. A dictation stops itself
after 10 minutes. A phone keyboard's own dictation works too and needs none of this.

Dictation calls `transcribe`, which turns audio into text without storing anything:

`POST /v1/transcribe`

```json
{"name": "memo.m4a", "audio_base64": "<base64 of memo.m4a>"}
```

It answers the text and the model used. Any signed-in caller may use it.

## Speech-to-Text That Is Not Configured

Speech-to-text needs a key on the lake you use. When it has none, recording and audio calls answer
`not_supported` with *Speech-to-text is not configured on this lake.* The app then turns speech off
for the tab: **Record** and **Upload audio** are disabled with that sentence, the list shows it as
a banner, the microphones hide, and **Upload text** and **Paste** still work.

## Permissions

| To | You need, on the account that owns the workspace |
|---|---|
| List, search and read transcripts | `read` |
| Add, edit and delete transcripts | `write` |
| Dictate (`transcribe`) | to be signed in |

You hold `admin` on your own workspaces. In an organization, owners and admins can add and change
transcripts; members read them unless they were granted `write`. A transcript of another workspace
of the same account is `not_found` here. Ask Orama reads transcripts with your own read rights, and
an agent's key reads them when its scope carries `list_transcripts` and `get_transcript`.

## Limits

| What | Limit |
|---|---|
| Audio, decoded | 25 MiB (OpenAI's upload limit) |
| Audio formats | webm, mp4, m4a, mp3, mpga, mpeg, wav, ogg, oga, flac |
| A recording in the browser | 60 minutes, or just under 25 MB |
| A text file you upload | 5 MB |
| Text kept, after cleaning | 400,000 bytes |
| Title | 1 to 300 bytes |
| `language` | letters and `-`, at most 16 characters |
| A dictation | 10 minutes |

Transcription happens while you wait. The proxies in front of lake close a response after about a
minute, so a very long recording can answer 504 at the edge even though lake finishes; short phone
recordings transcribe well within it. Split a long recording, or upload its text.

## Troubleshooting

| What you see | Why, and what to do |
|---|---|
| `not_supported` (501) | Speech-to-text is not configured on this lake. Use **Upload text** or **Paste**. |
| `engine_error` (422) | OpenAI could not transcribe the audio, is busy or over its quota, or could not be reached. The message says which; try again shortly. |
| `size_invalid` (413) at `/audio_base64` or `/text` | The audio is over 25 MiB or the text over 400,000 bytes. Split it. |
| `invalid_input` at `/text` | Neither text nor audio was sent, or no speech was heard in the audio. |
| `invalid_input` at `/title`, `/language`, `/recorded_at` or `/cursor` | That field is empty, too long or malformed. |
| `not_found` at `/id` | No such transcript in this workspace. |
| `unauthorized` (403) | You lack `write` on the account, or your key's scope does not carry the capability. |
| `workspace_not_ready` | The workspace is not ready yet. |
| The microphone does nothing | The browser needs permission, a secure (https) page and a tap. Check the site's microphone setting. |
| 504 on a long recording | See the limit above: split the recording. |

## Related Pages

- [Using Lake on a Phone](/docs/phone)
- [Ask Orama](/docs/ask-orama)
- [Search](/docs/search): transcripts are one of the places it looks
- [Workspaces](/docs/workspaces)
- [Security Model](/docs/security)
