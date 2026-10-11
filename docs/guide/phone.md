# Using Lake on a Phone

Lake works in your phone's browser: Safari on an iPhone, Chrome on Android. Every page fits the
screen without sideways scrolling, Ask Orama becomes a full-screen conversation, and you can speak
instead of typing. A phone is also the easiest way to capture a meeting: record it, or upload a
voice memo or a recorded call, and Lake turns it into a searchable transcript.

## Concepts

- **The same app.** There is nothing to install. Open your Lake's address in the browser and sign
  in; every page, permission and audit record is the same as on a computer.
- **A phone-shaped layout below 768 pixels wide.** The rail becomes a drawer, the top bar shows
  icons, dialogs you type into fill the screen with their main button above the keyboard, and
  tables scroll inside their own box.
- **Speech-to-text.** Dictation and recordings are turned into text by OpenAI with your Lake's own
  key. The audio is used for that one call and **never kept**. When your Lake has no
  speech-to-text key, the microphones are hidden and only the text ways stay offered.

## Find Your Way Around

- **The rail is a drawer.** Tap the menu button at the top left to open it, and its close button to
  close it. Choosing a page closes it.
- **The top bar is compact.** The breadcrumb shows only where you are; search, the workspace pill
  and Ask Orama are icons. Tap the workspace pill to see and switch workspaces; its menu names the
  current one ([Workspaces](/docs/workspaces)).
- **Search** opens **Go to…**, which finds any page.
- **Fields are large enough not to zoom.** Every field uses 16-pixel text, so Safari does not zoom
  the page when you tap it, and pinch-zoom still works.

## Ask Orama on a Phone

Tap the Ask Orama icon. It fills the screen, sized to the part of the screen the keyboard leaves,
with **Send** just above the keyboard.

- Return starts a new line; **Send** sends.
- The conversation follows new answers while you are at the end. If you scrolled up to read, it
  stays where you are and offers **New reply**.
- A link in an answer that opens a page closes Ask Orama, so the page shows.
- The microphone beside the box dictates ([Dictate Instead of Typing](#dictate-instead-of-typing)).
- **From your phone** explains how to bring in a voice memo or a recorded call.

Everything else is as on a computer: proposals you confirm, agents working in the background, the
conversation kept in the workspace ([Ask Orama](/docs/ask-orama)).

## Dictate Instead of Typing

The microphone is in Ask Orama's message box, and in the text of a new or edited transcript.

1. Tap the microphone. The first time, the browser asks for permission to use it; allow it.
2. Speak. Tap stop when you are done.
3. Lake transcribes what you said and adds it to the box, after anything already typed, to edit
   before you send or save.

A dictation stops by itself after 10 minutes. Your phone keyboard's own dictation key also works and
needs none of this.

Through the API, dictation is `POST /v1/transcribe` with the audio as base64, in the workspace
whose speech-to-text usage it is:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "name": "memo.m4a", "audio_base64": "<base64 audio>"}
```

Through MCP, call `transcribe` with the same input. It answers the `text`.

## Record a Meeting

1. Open **Data → Transcripts** and tap **New transcript**. It opens full screen.
2. Tap **Record**, then the red microphone (**Start recording**). Keep the page open while you
   record.
3. Tap **Stop recording** when done (or cancel to discard it). Listen back if you like.
4. Name it (the title starts as the recording's time) and tap **Transcribe and save**. Lake
   transcribes it, keeps the text, drops the audio, and opens the new transcript.

An iPhone records `.m4a`, Android Chrome `.webm`. A recording stops itself after 60 minutes or just
before 25 MB.

## Upload a Voice Memo or a Recorded Call

First save the recording to your phone's files. **From your phone**, beside the choices, shows these
steps for the phone you are using:

**iPhone**

- *A voice memo:* in Voice Memos, open the recording, tap **•••** or Share, then **Save to Files**
  (On My iPhone or iCloud Drive, in Downloads).
- *A call you recorded:* in Notes, open **Call Recordings**, then the call's note, tap **•••**, and
  save or share the audio to **Files**.

**Android**

- *A voice recording:* in Recorder, Voice Recorder (Samsung) or Sound Recorder, open the recording,
  tap **Share** (or ⋮) and save it to Files or My Files, in **Download**.
- *A call you recorded:* in the Phone app, open Recents, the contact, **History**, then the call,
  and tap **Share** to Files or Drive. A file manager cannot see it until you share it.

Then in Lake:

1. **Data → Transcripts → New transcript → Upload audio**.
2. Choose the file from Files (on an iPhone) or Files or My Files (on Android).
3. Name it and tap **Transcribe and save**.

Text files work the same way through **Upload text**: `.txt`, `.md`, and `.vtt` or `.srt` captions,
whose timings Lake removes. **Paste** takes text you type, paste or dictate.

Through the API, a transcript from audio is `POST /v1/create_transcript`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "title": "Weekly sync", "audio_base64": "<base64 audio>", "audio_name": "memo.m4a", "source": "upload"}
```

and from text:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "title": "Call notes", "text": "Dana: we renew in March.", "source": "text"}
```

Through MCP, call `create_transcript` with the same inputs. More: [Transcripts and
Speech-to-Text](/docs/transcripts).

## Ask About a Transcript

Open the transcript and tap **Ask Orama about this**. Ask Orama opens full screen with the
transcript attached to your next message, so you can ask *what did we agree?* or *list the action
items*.

## Permissions

The same as on any device. Creating a transcript and sending a message to Ask Orama need `write`
on the account that owns the workspace; reading them needs `read`.

## Limits

- Audio is at most 25 MB, a text file at most 5 MB. A larger file is refused before it is sent.
- Lake transcribes while you wait. The proxies in front of Lake may close a long wait sooner than
  Lake does, so a long recording can answer a timeout; short phone recordings transcribe well
  within it.
- A phone held sideways is wider than 768 pixels and gets the computer layout.
- The system Back gesture goes back a page; it does not close an open sheet. Use the sheet's own
  Back or close button.
- The microphone needs a secure (`https://`) address and a tap to start.

## Troubleshooting

| You see | What to do |
|---|---|
| *Speech-to-text is not configured on this Lake.* | Your Lake has no speech-to-text key. **Upload text** and **Paste** still work; ask your Lake's operator to add the key. |
| The browser never asks for the microphone, or refuses it | Allow the microphone for your Lake's site in the browser's settings, then reload. |
| `size_invalid` (413) | The audio is over 25 MB or the text over 400,000 bytes. Split the recording. |
| `engine_error` (422) | OpenAI could not transcribe it; the message says why. Try again, or upload the text. |
| A timeout while transcribing a long recording | The proxy closed the wait. Use a shorter recording. |
| The file picker does not show your recording | Save it to Files first (see **From your phone** above). |

## Related Pages

- [Transcripts and Speech-to-Text](/docs/transcripts)
- [Ask Orama](/docs/ask-orama)
- [Workspaces](/docs/workspaces)
- [Your Account and Sign-In](/docs/account)
