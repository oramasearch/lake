# Lake on a Phone

How lake's web app works in a phone's browser (lake#561). The mechanisms are ported from
`oramasearch/rev-os` (its `docs/engineering/mobile-web.md`) and drawn in lake's look. Read this
before building anything a person types into on a phone: a dialog, a composer, a form. Each rule
says what, where the code is, and why. The why is usually a browser quirk that is invisible on a
desktop and obvious on an iPhone.

## The Shared Pieces

Reuse these; don't rewrite them.

| Piece | Where | What it does |
|---|---|---|
| `useIsPhone`, `useCoarsePointer`, `useMedia`, `PHONE_QUERY` | `frontend/src/lib/phoneSheet.ts` | below 768 px; a touch screen |
| `usePhoneSheet(active, reveal?)` | `lib/phoneSheet.ts` | while active: `--vv-top` and `--vv-height` from the visual viewport, the page behind locked; returns the keyboard's height; with a scroller ref, reveals the focused field |
| `Modal sheet` and `SheetActions` | `frontend/src/lib/overlay.tsx` | a dialog that is a full-screen sheet on a phone, with a pinned footer for its main action; unchanged on a wide screen |
| `data-keep-visible`, `data-pinned-top` | `lib/phoneSheet.ts` (`KEEP_VISIBLE`, `PINNED_TOP`) | reveal a group (a box and its button) together; keep clear of a sticky header |
| `createPageLock`, `revealBy` | `frontend/src/lib/sheet.ts` | the pure logic: the counted lock, how far to scroll (`tests/sheet.test.ts`) |
| `keyboardInset`, `sheetFrame`, `nearBottom`, `onConversationGrew` | `frontend/src/lib/phone.ts` | the keyboard's height; following a conversation (`tests/phone.test.ts`) |
| `pickRecordingFormat`, `extensionFor`, `AUDIO_ACCEPT`, `TEXT_ACCEPT`, `fileProblem`, `phoneKind` | `lib/phone.ts` | recording formats, the file pickers, which phone |
| `useFollow`, `NewReplyChip` | `frontend/src/components/follow.tsx` | a conversation that follows new content unless the person scrolled up |
| `useRecorder`, `DictateButton`, `RecordingBar`, `DictationError` | `frontend/src/components/dictation/Dictate.tsx` | the microphone, 44 px on a phone ([transcripts.md](transcripts.md#dictation)) |
| `FromYourPhoneLink`, `FromYourPhonePanel` | `frontend/src/components/phone/FromYourPhone.tsx` | how to find a voice memo or a recorded call on an iPhone or Android, opened in place; its tabs and Close are 44 px on a phone |
| `.hit`, `.plain`, the phone rules | `frontend/src/styles/phone.css` | touch targets, 16 px fields, tables in their own scroller, sheets |

Users today: Ask Orama (`components/AskPanel.tsx`, `usePhoneSheet` directly, its own layout), New
Transcript and Edit Transcript (`Modal sheet`). A new dialog a person writes in on a phone should be
a `Modal sheet` with its submit in `SheetActions`. Because `SheetActions` moves the buttons out of
the form on a phone, the submit button names its form (`form={id}`).

## 1. Viewport and Keyboard

**The viewport meta** (`frontend/index.html`):
`width=device-width, initial-scale=1.0, viewport-fit=cover, interactive-widget=resizes-content`.

- `viewport-fit=cover` lets the page reach under the notch and the home indicator, so a sheet can
  paint edge to edge, which makes the safe-area insets our job (§2).
- `interactive-widget=resizes-content` asks Android Chrome to shrink the layout viewport when the
  keyboard opens, so `100dvh` and `position: fixed; bottom: 0` follow the keyboard there. iOS
  Safari ignores it.

**iOS Safari never resizes the layout viewport for the keyboard.** It shrinks the visual viewport
and may scroll it (`visualViewport.offsetTop`). A `fixed` element at `bottom: 0` or a box of
`100dvh` ends up behind the keyboard. So a sheet is sized to the visual viewport: `usePhoneSheet`
writes `--vv-top` and `--vv-height` on `<html>` on every `visualViewport` `resize` and `scroll`
(and `window` `resize`), and the sheet is `top: var(--vv-top, 0px); height: var(--vv-height,
100dvh)` (`.dialog-sheet` and `.ask-panel` in the phone rules). The values are CSS variables, not
React state, so the box follows each frame of the keyboard without re-rendering.

**The keyboard's height** is `layoutHeight − visualHeight − offsetTop` (`keyboardInset`). Under
80 px it is the URL bar collapsing, not a keyboard. On Android it reads 0, which is right.

**Keep the submit above the keyboard.** Whatever is pinned to the sheet's bottom sits just above the
keyboard once the sheet is the visual viewport. So the main action lives in a footer outside the
scroller: Ask Orama's message box, and New Transcript's **Save** through `SheetActions`.

**Bring the focused field into view.** iOS scrolls a focused field into view against the old
layout. `usePhoneSheet(active, scrollerRef)` re-reveals after every viewport change and on
`focusin`, scrolling only the sheet's scroller by `revealBy`. Mark a group `data-keep-visible` (the
transcript's text with its microphone) to reveal the group.

## 2. The Page Behind

**Lock it, and put it back exactly.** `overflow: hidden` on `<body>` does not stop iOS scrolling the
page under a fixed overlay. The lock (`createPageLock`) pins the body at `top: -scrollY`, sets
`overflow: hidden` on `<html>` and `data-sheet="open"` (which turns on `overscroll-behavior: none`).
On close it restores every property, removes the viewport variables and scrolls back. The lock is
counted: a sheet over another doesn't unlock the page when it closes first.

**Safe areas.** The sheet's header pads `max(10px, env(safe-area-inset-top))`, its sides
`max(16px, …left/right)`, and its bottom the home indicator's inset only while the keyboard is
down: the Modal and Ask Orama set `--sheet-safe-bottom` to `0px` or `env(safe-area-inset-bottom)`
from the keyboard height. The body pads its left and right insets, and the rail drawer its top and
bottom.

**One scroller per sheet**: `min-height: 0; flex: 1; overflow-y: auto; overflow-x: hidden;
overscroll-behavior: contain`.

**Keeping a place.** Ask Orama follows new answers while the person is at the end, stays put when
they scrolled up, and offers **New reply** (`useFollow`, 80 px slack). A `ResizeObserver` keeps the
end in view when the keyboard shrinks the scroller. Their own question always brings them down.

**One tree across the breakpoint.** A phone turned sideways is 844 px wide. `Modal` renders one
tree and switches classes, so rotating never loses what was typed.

## 3. The Shell on a Phone

- **The top bar is compact**: the breadcrumb shows its last level only; search, the workspace pill
  and Ask Orama are icons (the workspace menu names the workspace); every icon has a 40 px hit area.
- **The rail is a drawer** below 1024 px, opened from the menu button, with Close above the brand.
- **Ask Orama is a full-screen conversation**, sized to the visual viewport, above the shell. A link
  in it that navigates closes it, so the page shows.

## 4. Inputs and Touch

- **16 px type in anything typed into** (`phone.css`, below 768 px). iOS Safari zooms the page on a
  smaller field and doesn't zoom back. Never fix it with `maximum-scale=1`, which takes pinch-zoom
  from people who need it.
- **Fields are at least 40 px tall** below 1024 px; a select draws its own chevron on a phone, since
  iOS cuts 16 px text off in a native one.
- **Touch targets.** Every control has an invisible hit box: at least 24 px everywhere, 40 px on the
  kit's and the shell's controls (`.hit`), below 1024 px; the visual size stays. What is tapped
  often (send, the microphone, close, a sheet's actions, From your phone's tabs) is 44 px, written
  in px. The hit rule makes links and buttons `position: relative`; an absolutely placed control
  that an earlier sibling now covers needs a `z-index` (the drawer's Close does).
- **`.plain`** takes back lake's button styling (look.css styles every button in `main` and
  dialogs) so Tailwind utilities alone draw a control: the microphone, the choice cards.
- **Return on a touch screen makes a new line**; Send sends (`useCoarsePointer`, `enterKeyHint`).
  Enter is ignored while `isComposing`.
- **Nothing scrolls sideways.** Tables scroll inside their own box (`.table-wrap`, and a bare
  `main table` becomes its own scroller); prose breaks long words (`overflow-wrap`); a checkbox in a
  filter row keeps its size.

## 5. Media

- **Recording:** iOS Safari's MediaRecorder makes MP4/AAC (`.m4a`), Chrome and Firefox WebM/Opus.
  `pickRecordingFormat` takes the first supported format; `extensionFor` names the file after the
  type actually produced. The microphone needs a secure origin and a tap.
- **The pickers:** Upload audio's `accept` lists extensions and `audio/*` (`AUDIO_ACCEPT`), so an
  iPhone offers Files, where a voice memo saved with **Save to Files** is. Upload text takes `.txt`,
  `.md`, `.vtt` and `.srt` (`TEXT_ACCEPT`). **From your phone** says how to get a voice memo or a
  recorded call there, for the phone in use.

## How to Check It

**Unit:** `npm test` runs `tests/phone.test.ts`, `tests/sheet.test.ts`, `tests/dictation.test.ts`
and `tests/transcripts.test.ts`.

**At 390×844 in a browser**, with no lake:

```sh
cd frontend
LAKE_ORIGIN=http://127.0.0.1:9 npx vite --port 5174 &
PLAYWRIGHT_FROM=<a package.json beside a playwright install> OUT=/tmp/shots node checks/phone.mjs
ENGINE=webkit … node checks/phone.mjs      # WebKit with iPhone emulation, as rev-os requires
VISITOR=1 … node checks/phone.mjs          # the signed-out shell
```

`checks/phone.mjs` emulates an iPhone 13 (390×844, iPhone user agent, touch, DPR 3), answers every
`/v1` call from its mocks (a signed-in viewer with one workspace, three transcripts), and reports for
each page: the document's width (it must be 390), visible controls whose hit area is under 24 px,
and fields under 16 px or 40 px. Then it checks the top bar, the drawer, New Transcript (the sheet
fills the screen and locks the page; with the keyboard simulated as iOS does it, `visualViewport`
508 px, Save sits above it; the page is unlocked after), From your phone's 44 px tabs, and Ask
Orama opened from a transcript (full screen, the context chip and draft, 16 px box, following the
latest answer, Send above the keyboard, and the transcript sent as `page`). `PAGES=/a,/b` sweeps
other paths; screenshots land in `OUT`.

In Chrome by hand: DevTools → device toolbar → iPhone 12 Pro (390×844).

**What still needs a real phone** (an iPhone with Safari and an Android phone with Chrome):

- the real keyboard's animation, its suggestion bar and the emoji keyboard changing height;
- iOS's own focus scrolling and the URL bar collapsing as the page scrolls;
- safe areas on a notched phone, in portrait and landscape;
- the microphone permission prompt, recording with the screen locked, Bluetooth headsets;
- the file picker's real sources and a 25 MB upload on a cellular connection.

## Known Limits

- A phone held sideways is wider than 768 px and gets the wide layout.
- The system Back gesture navigates rather than closing an open sheet.
- `vite`'s dev proxy sends `/drive/preview` to lake, so that page can't be checked without one.
