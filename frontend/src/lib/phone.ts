/** Lake on a phone, as plain logic, ported from rev-os's `lib/phone.ts` (lake#561). No React and no
 * DOM, so `npm test` covers it: which recording format a browser can make, how far the on-screen
 * keyboard covers the page, when a conversation follows a new answer, which files a transcript
 * can be made from, and which phone the person holds. docs/ui/mobile-web.md explains each rule. */

/** A recording format a browser's MediaRecorder can make, and the file extension it is sent as. */
export type AudioFormat = { mime: string; ext: string };

/** Chrome and Firefox record WebM/Opus; iOS Safari records MP4/AAC (.m4a). Order matters: the
 * first the browser supports is used. */
export const RECORDING_FORMATS: AudioFormat[] = [
  { mime: "audio/webm;codecs=opus", ext: "webm" },
  { mime: "audio/webm", ext: "webm" },
  { mime: "audio/mp4", ext: "m4a" },
  { mime: "audio/mp4;codecs=mp4a.40.2", ext: "m4a" },
  { mime: "audio/ogg;codecs=opus", ext: "ogg" },
];

export function pickRecordingFormat(isTypeSupported: (mime: string) => boolean): AudioFormat | null {
  for (const f of RECORDING_FORMATS) {
    try {
      if (isTypeSupported(f.mime)) return f;
    } catch {
      /* a browser that throws on an unknown type: try the next */
    }
  }
  return null;
}

/** The extension for whatever type the recorder actually produced (it may differ from the one
 * asked for); unknown types fall back to the asked-for format. */
export function extensionFor(mime: string, fallback: AudioFormat | null): string {
  const m = (mime || "").toLowerCase();
  if (m.includes("webm")) return "webm";
  if (m.includes("mp4") || m.includes("aac") || m.includes("m4a")) return "m4a";
  if (m.includes("ogg")) return "ogg";
  if (m.includes("wav")) return "wav";
  if (m.includes("mpeg") || m.includes("mp3")) return "mp3";
  return fallback?.ext ?? "webm";
}

/** How much of the layout viewport the on-screen keyboard covers, in px (0 when closed). iOS
 * Safari shrinks only the visual viewport and may scroll it (offsetTop); Android Chrome may resize
 * the layout viewport itself, and then this stays 0 and `100dvh` follows. */
export function keyboardInset(layoutHeight: number, visualHeight: number, visualOffsetTop: number): number {
  const inset = Math.round(layoutHeight - visualHeight - visualOffsetTop);
  // Below ~80 px it is browser chrome (the URL bar collapsing), not a keyboard.
  return inset > 80 ? inset : 0;
}

/** Where a sheet goes: the visual viewport's top and height (the layout viewport where the browser
 * has no visualViewport), and the keyboard's height, 0 when closed. */
export function sheetFrame(vv: { height: number; offsetTop: number } | null | undefined, innerHeight: number): { top: number; height: number; keyboard: number } {
  const height = vv ? vv.height : innerHeight;
  const top = vv ? vv.offsetTop : 0;
  return { top: Math.round(top), height: Math.round(height), keyboard: keyboardInset(innerHeight, height, top) };
}

/** Whether a scroller is (close enough to) its end to keep following new content. */
export function nearBottom(scrollTop: number, clientHeight: number, scrollHeight: number, slack = 80): boolean {
  return scrollHeight - (scrollTop + clientHeight) <= slack;
}

/** After the conversation grows: follow it when the person was at the end, or when they just asked
 * (their own question always brings them down); otherwise show the "New reply" chip. */
export function onConversationGrew(wasAtBottom: boolean, askedJustNow: boolean): "follow" | "chip" {
  return wasAtBottom || askedJustNow ? "follow" : "chip";
}

/** 0:07, 1:32, 12:05 */
export function clock(seconds: number): string {
  const s = Math.max(0, Math.floor(seconds));
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, "0")}`;
}

/** A dictation stops itself at this length; the transcription limit stays far away. */
export const MAX_DICTATION_SECONDS = 10 * 60;
/** A transcript's recording stops itself at this length, or when it nears the size limit. */
export const MAX_RECORDING_SECONDS = 60 * 60;
/** What `transcribe` and `create_transcript` take: at most 25 MB of audio. */
export const MAX_AUDIO_BYTES = 25 * 1024 * 1024;
/** A text file made into a transcript. */
export const MAX_TEXT_BYTES = 5 * 1024 * 1024;
/** A recording stops a little before the limit, so the last second's chunk still fits. */
export const RECORDING_STOP_BYTES = MAX_AUDIO_BYTES - 512 * 1024;

/** Whether a recording must stop now: at its length limit or near the size limit. */
export function recordingFull(bytes: number, seconds: number, maxSeconds: number): boolean {
  return bytes >= RECORDING_STOP_BYTES || seconds >= maxSeconds;
}

// -------------------------------------------------------------- what a transcript is made from

export type FileKind = "audio" | "text";

const KINDS: Record<string, FileKind> = {
  mp3: "audio",
  wav: "audio",
  m4a: "audio",
  mp4: "audio",
  aac: "audio",
  webm: "audio",
  weba: "audio",
  ogg: "audio",
  oga: "audio",
  opus: "audio",
  flac: "audio",
  mpeg: "audio",
  mpga: "audio",
  // Android's recorders.
  amr: "audio",
  "3gp": "audio",
  "3ga": "audio",
  "3gpp": "audio",
  txt: "text",
  text: "text",
  md: "text",
  markdown: "text",
  vtt: "text",
  srt: "text",
};

/** Every recording format lake takes for a transcript, in words. */
export const ACCEPTED_RECORDINGS = "M4A or MP4, MP3, WAV, AAC, FLAC, AMR, 3GP, WebM or Ogg";
/** Every text format, in words. */
export const ACCEPTED_TEXT = "TXT, Markdown, WebVTT (.vtt) or SubRip (.srt)";

function accept(kind: FileKind, extra: string[]): string {
  return [
    ...Object.entries(KINDS)
      .filter(([, k]) => k === kind)
      .map(([e]) => `.${e}`),
    ...extra,
  ].join(",");
}

/** The audio picker's accept list: extensions and `audio/*`, so an iPhone offers Files (where a
 * saved voice memo is) and Android its recorder's folder. */
export const AUDIO_ACCEPT = accept("audio", ["audio/*"]);
/** The text picker's accept list. */
export const TEXT_ACCEPT = accept("text", ["text/plain", "text/markdown", "text/vtt"]);

export function kindOf(name: string): FileKind | null {
  const ext = name.includes(".") ? name.split(".").pop()!.toLowerCase() : "";
  return KINDS[ext] ?? null;
}

/** Why a file can't be made into a transcript of this kind, in words, or null when it can. The
 * server checks again; this only saves an upload that would be refused. */
export function fileProblem(file: { name: string; size: number }, want: FileKind): string | null {
  const kind = kindOf(file.name);
  if (kind !== want) {
    return want === "audio" ? `That isn't a recording lake takes. Choose ${ACCEPTED_RECORDINGS}.` : `That isn't a text file lake takes. Choose ${ACCEPTED_TEXT}.`;
  }
  if (file.size === 0) return "That file is empty.";
  const limit = want === "audio" ? MAX_AUDIO_BYTES : MAX_TEXT_BYTES;
  if (file.size > limit) return `That file is over the ${Math.round(limit / 1024 / 1024)} MB limit.`;
  return null;
}

// -------------------------------------------------------------- which phone

export type PhoneKind = "iphone" | "android";

/** Which phone the person is using, from the browser's user agent: an iPhone (or an iPad, which
 * reports itself as a Mac with a touch screen), Android, or null on a computer. */
export function phoneKind(userAgent: string, maxTouchPoints = 0): PhoneKind | null {
  const ua = userAgent || "";
  if (/android/i.test(ua)) return "android";
  if (/iphone|ipad|ipod/i.test(ua)) return "iphone";
  if (/macintosh/i.test(ua) && maxTouchPoints > 1) return "iphone";
  return null;
}
