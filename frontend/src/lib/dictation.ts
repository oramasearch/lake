/** Dictation, shared by Ask Orama's message box and the Transcripts page (lake#561), ported from
 * rev-os. Pure functions and one small session switch; `npm test` covers them
 * (tests/dictation.test.ts). */

/** Dictated text goes after what is already typed, separated by one space. */
export function appendDictation(prev: string, said: string): string {
  const text = said.trim();
  if (!text) return prev;
  return prev.trim() ? `${prev.trimEnd()} ${text}` : text;
}

/** "1 min 12 s", "9 s", "1 h 2 min": a recording's length in words; "" when unknown. */
export function spokenLength(seconds: number | null | undefined): string {
  if (!seconds || seconds < 0) return "";
  const total = Math.round(seconds);
  if (total >= 3600) {
    const minutes = Math.floor((total % 3600) / 60);
    return `${Math.floor(total / 3600)} h${minutes ? ` ${minutes} min` : ""}`;
  }
  if (total >= 60) return `${Math.floor(total / 60)} min ${total % 60} s`;
  return `${total} s`;
}

/** What lake answers when it has no speech-to-text key. */
export const NOT_CONFIGURED = "Speech-to-text is not configured on this Lake.";

/** Whether a failed `transcribe` or `create_transcript` means speech-to-text is not configured. */
export function isNotSupported(error: unknown): boolean {
  return !!error && typeof error === "object" && (error as { code?: unknown }).code === "not_supported";
}

/** What the person is told, once, when the microphone goes away for the session. */
export const MICROPHONE_GONE = `${NOT_CONFIGURED} The microphone is hidden for now; typing and your keyboard's own dictation still work.`;

// -------------------------------------------------------------- the session switch

const KEY = "lake-speech-to-text";
let off: boolean | null = null;
const listeners = new Set<() => void>();

/** Whether this tab has learned that the lake has no speech-to-text. */
export function speechToTextOff(): boolean {
  if (off === null) {
    try {
      off = sessionStorage.getItem(KEY) === "off";
    } catch {
      off = false;
    }
  }
  return off;
}

/** Remember for this tab that the lake has no speech-to-text; every microphone hides. */
export function turnSpeechToTextOff() {
  off = true;
  try {
    sessionStorage.setItem(KEY, "off");
  } catch {
    /* storage blocked: it lasts while the page is open */
  }
  listeners.forEach((l) => l());
}

export function onSpeechToTextChange(listener: () => void): () => void {
  listeners.add(listener);
  return () => {
    listeners.delete(listener);
  };
}

/** `transcribe`'s input for a dictation in the workspace a person works in (lake#613): its audio
 * minutes are that workspace's speech-to-text usage, so a dictation names it. Null without a
 * workspace: nothing to bill it to, so the microphone does not offer dictation there. */
export function dictationInput(
  scope: { customerId: string; workspaceId: string },
  name: string,
  audio: string,
): { customer_id: string; workspace_id: string; name: string; audio_base64: string } | null {
  if (!scope.customerId || !scope.workspaceId) return null;
  return { customer_id: scope.customerId, workspace_id: scope.workspaceId, name, audio_base64: audio };
}
