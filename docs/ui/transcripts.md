# Transcripts

Data → Transcripts (lake#561): conversations, voice memos and calls as text a person can search,
edit and ask Orama about, built phone-first so a recording made on a phone becomes a transcript from
the same phone. The pages are in `frontend/src/pages/transcripts/`; the phone mechanics are in
[mobile-web.md](mobile-web.md).

## The Pages

| Route | File | Shows |
|---|---|---|
| `/w/<id>/transcripts?q=` | `TranscriptsPage.tsx` | figures (how many, recorded length, latest), search by text, one card per transcript: title, date, excerpt, source chip, length, who added it; **Show more** pages by cursor; **New transcript** |
| `/w/<id>/transcripts?new=1` | the same | opens New Transcript at once |
| `/w/<id>/transcript?id=<id>` | `TranscriptPage.tsx` | the title and text, figures (words, length, source, recorded), **Ask Orama about this**, **Copy**, **Edit**, **Delete** |

Both are workspace pages (`workspace: true`): their unprefixed addresses open the current workspace,
and both sit behind the `WorkspaceGate`. One transcript is `/transcript?id=`, the address style of
lake's other detail pages (`/train/dataset`, `/repository?id=`), so it is linkable and `isWorkspacePath`
knows both. The rail lists Transcripts in Data, after Datasets. Start Your Workspace from either
page lands back on Transcripts.

## New Transcript

A `Modal sheet` (`NewTranscript.tsx`): full screen on a phone with Back and Save above the keyboard.
It offers four ways in:

| Way | What the person does | `create_transcript` |
|---|---|---|
| **Record** | taps the red microphone, then Stop (or Cancel, which discards it), hears it back, names it | `audio_base64`, `audio_name` (`recording.m4a` or `.webm`), `recorded_at`, `source: "recording"` |
| **Upload audio** | picks a recording from Files or Voice Memos (`AUDIO_ACCEPT`) | `audio_base64`, `audio_name`, `source: "upload"` |
| **Upload text** | picks a `.txt`, `.md`, `.vtt` or `.srt` file, read in the browser and previewed | `text`, `source: "upload"`; lake cleans caption timings |
| **Paste** | types, pastes or dictates the text | `text`, `source: "text"` |

The title starts from the file name, the recording's time ("Recording, Oct 9, 14:05") or the first
line of pasted text (`lib/transcripts.ts`). A recording stops itself after 60 minutes or just before
25 MB; a file over 25 MB (audio) or 5 MB (text), empty, or of another kind is refused before upload
(`fileProblem`). Audio goes as a `data:` URL. While lake transcribes, the sheet shows the elapsed
time and says to keep it open. On success the new transcript's page opens.

**From your phone** (beside the choices and Upload audio) says how to save a voice memo or a
recorded call to Files on an iPhone or Android and pick it here.

## Speech-to-Text Not Configured

When the lake has no speech-to-text key, `transcribe` and `create_transcript` with audio answer
`not_supported` ("Speech-to-text is not configured on this lake."). The app then turns speech off
for the tab (`sessionStorage["lake-speech-to-text"] = "off"`, `lib/dictation.ts`): Record and Upload
audio are disabled with that sentence, the list page shows it as a banner, the text ways stay
offered, and every dictation microphone hides. The person is told once, where it happened.

## Dictation

The microphone (`components/dictation/Dictate.tsx`, rev-os's `useRecorder`, `DictateButton` and
`RecordingBar`) records with MediaRecorder (iOS MP4, others WebM) and sends the recording to
`transcribe` `{name, audio_base64}` through `act`; the answer's `text` is added to the box, after
what is typed, to edit or send. It is in:

- Ask Orama's message box (the panel and the full page), with **From your phone** beside it;
- Paste a Transcript and Edit Transcript, where it adds to the text.

A dictation stops itself after 10 minutes. The phone keyboard's own dictation needs none of this.

## The Transcript Page

- **Ask Orama about this** opens the Ask Orama panel (full screen on a phone) through `openAsk`
  (`lib/ask.ts`). The panel shows a context chip ("Transcript: <title>", dismissable) and puts
  "About the transcript “<title>”: " in the box. The next message is sent with `page` set to
  `transcriptAskPage` (path `/transcript`, `id`, `kind: "transcript"`, `title`, and `transcript`:
  the text, cut at 20,000 characters, saying so), in place of the page's own path. After that
  message the chip goes and later messages are about the page as usual.
- **Copy** copies the text.
- **Edit** opens a `Modal sheet` with the title and text (and the microphone); Save sends only what
  changed to `update_transcript`.
- **Delete** asks "Delete “<title>”?" and says the transcript and its text are removed from the
  workspace; it calls `delete_transcript` and returns to the list.

## Capabilities

| Capability | Kind | Input the app sends |
|---|---|---|
| `list_transcripts` | read | `customer_id`, `workspace_id`, `q`, `limit` (50), `cursor` |
| `get_transcript` | read | `customer_id`, `workspace_id`, `id` |
| `create_transcript` | action | `customer_id`, `workspace_id`, `title`, `source`, and `text` or `audio_base64` + `audio_name` (+ `recorded_at`) |
| `update_transcript` | action | `customer_id`, `workspace_id`, `id`, `title?`, `text?` |
| `delete_transcript` | action | `customer_id`, `workspace_id`, `id` |
| `transcribe` | action | `name`, `audio_base64` |

The server side (lake#561) owns the records in the workspace Zetta, the transcription, the 25 MB
limit and the caption cleaning; this page documents only what the app sends and shows. `language`
is accepted by the server and not asked for by the app yet.

## How to Verify

- `npm test` covers the helpers (`tests/transcripts.test.ts`, `tests/phone.test.ts`,
  `tests/dictation.test.ts`).
- `checks/phone.mjs` walks both pages at 390×844, creates a pasted transcript through the sheet with
  the keyboard simulated, and opens Ask Orama about a transcript ([mobile-web.md](mobile-web.md#how-to-check-it));
  `STT=off` answers `not_supported`.
- Against a lake with the capabilities: record on a phone, upload a voice memo from Files, upload a
  `.vtt`, paste, search, edit, ask, delete.
