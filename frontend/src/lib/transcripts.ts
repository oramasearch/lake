// SPDX-License-Identifier: MIT

/** Transcripts (lake#561) as plain logic: the record's shape, the words a page shows for it, a
 * title from a file or a time, and the context Ask Orama is given about one. `npm test` covers it
 * (tests/transcripts.test.ts). The capabilities are `list_transcripts`, `get_transcript`,
 * `create_transcript`, `update_transcript` and `delete_transcript` (docs/ui/transcripts.md). */

export type TranscriptSource = "recording" | "upload" | "text";

/** One row of `list_transcripts`. */
export type TranscriptRow = {
  id: string;
  title: string;
  excerpt?: string;
  source?: TranscriptSource | string;
  seconds?: number | null;
  created_at?: string;
  created_by?: string;
  /** The author's display name, or null when lake has none (lake#579). */
  created_by_name?: string | null;
};

/** `get_transcript`, `create_transcript` and `update_transcript` answer the whole record. */
export type Transcript = TranscriptRow & {
  text: string;
  language?: string | null;
  recorded_at?: string | null;
};

const SOURCE_LABELS: Record<string, string> = { recording: "Recorded", upload: "Uploaded", text: "Pasted" };

/** "Recorded", "Uploaded", "Pasted": how a transcript was made, in words. */
export function sourceLabel(source: string | null | undefined): string {
  return SOURCE_LABELS[source ?? ""] ?? "Transcript";
}

/** A title from a file's name: its extension gone, separators as spaces ("call_2026-10-09.m4a" →
 * "call 2026-10-09"). */
export function titleFromFile(name: string): string {
  const base = name.replace(/\.[^./\\]+$/, "");
  return base.replace(/[_]+/g, " ").replace(/\s+/g, " ").trim() || "Untitled transcript";
}

/** A recording's default title: "Recording, Oct 9, 14:05" in the person's time zone. */
export function recordingTitle(at: Date): string {
  const day = at.toLocaleDateString("en-US", { month: "short", day: "numeric" });
  const time = `${String(at.getHours()).padStart(2, "0")}:${String(at.getMinutes()).padStart(2, "0")}`;
  return `Recording, ${day}, ${time}`;
}

/** A pasted transcript's default title: its first line, shortened. */
export function titleFromText(text: string, max = 60): string {
  const first = text.split(/\r?\n/).map((line) => line.trim()).find(Boolean) ?? "";
  if (!first) return "";
  return first.length > max ? `${first.slice(0, max - 1).trimEnd()}…` : first;
}

/** A row's excerpt, or the start of the text where the server sent none. */
export function excerptOf(row: { excerpt?: string; text?: string }, max = 180): string {
  const source = (row.excerpt ?? row.text ?? "").replace(/\s+/g, " ").trim();
  return source.length > max ? `${source.slice(0, max - 1).trimEnd()}…` : source;
}

/** Words in a text, for the transcript page's figures. */
export function wordCount(text: string): number {
  const words = text.trim().match(/\S+/g);
  return words ? words.length : 0;
}

/** How much of a transcript Ask Orama is given with the page it was asked from. */
export const ASK_CONTEXT_CHARS = 20_000;

/** The page Ask Orama is told about when asked about a transcript: its path and id, its title, and
 * its text (cut at ASK_CONTEXT_CHARS, saying so). `send_ask_message`'s `page` takes string values. */
export function transcriptAskPage(t: { id: string; title: string; text: string }): Record<string, string> {
  const text = t.text.length > ASK_CONTEXT_CHARS ? `${t.text.slice(0, ASK_CONTEXT_CHARS)}\n\n[The transcript continues; only its first ${ASK_CONTEXT_CHARS} characters are included.]` : t.text;
  return { path: "/transcript", id: t.id, kind: "transcript", title: t.title, transcript: text };
}

/** Who added a transcript, in words (lake#579): "you" for the person viewing, else the author's
 * display name. A raw user id is never shown: with no name it says nothing. */
export function authorLabel(row: { created_by?: string; created_by_name?: string | null }, viewerUserId?: string | null): string {
  if (row.created_by && viewerUserId && row.created_by === viewerUserId) return "you";
  return (row.created_by_name ?? "").trim();
}
