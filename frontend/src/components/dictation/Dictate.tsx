/** The microphone, ported from rev-os's `components/dictation/Dictate.tsx` (lake#561). Ask Orama's
 * message box and the Transcripts page use it.
 *
 * `useRecorder` records with MediaRecorder (iOS Safari MP4, Chrome and Firefox WebM/Opus). For
 * dictation (`onText`) it sends the recording to lake's `transcribe` as base64 JSON and hands back
 * the text to edit; for a transcript (`onAudio`) it hands back the recording itself. Cancelling
 * discards the recording without sending it. A dictation names the workspace the person works in,
 * whose speech-to-text usage it is (lake#613); without one there is no dictation. When lake answers
 * `not_supported` (no speech-to-text
 * key), every dictation microphone hides for the rest of the session and the person is told why
 * once. The phone keyboard's own dictation needs none of this: it types into the field. */
import { useEffect, useRef, useState } from "react";
import { Loader2, Mic, Square, X } from "lucide-react";
import { act, ApiError } from "@/lib/api";
import { cx } from "@/lib/ui";
import { MAX_DICTATION_SECONDS, clock, extensionFor, pickRecordingFormat, recordingFull, type AudioFormat } from "@/lib/phone";
import { MICROPHONE_GONE, dictationInput, isNotSupported, onSpeechToTextChange, speechToTextOff, turnSpeechToTextOff } from "@/lib/dictation";
import { useSession } from "@/lib/session";

type RecState = "idle" | "recording" | "transcribing";

/** A Blob as a `data:` URL, which `transcribe` and `create_transcript` accept as `audio_base64`. */
export function blobToDataUrl(blob: Blob): Promise<string> {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve(String(reader.result ?? ""));
    reader.onerror = () => reject(reader.error ?? new Error("The recording could not be read."));
    reader.readAsDataURL(blob);
  });
}

/** Whether this lake can turn speech into text, as far as this tab has learned. */
export function useSpeechToText(): boolean {
  const [off, setOff] = useState(speechToTextOff);
  useEffect(() => {
    return onSpeechToTextChange(() => setOff(speechToTextOff()));
  }, []);
  return !off;
}

/** A finished recording, for a transcript. */
export type Recording = { blob: Blob; name: string; seconds: number };

export function useRecorder({ onText, onAudio, maxSeconds = MAX_DICTATION_SECONDS }: { onText?: (text: string) => void; onAudio?: (recording: Recording) => void; maxSeconds?: number }) {
  const canRecord = typeof window !== "undefined" && !!navigator.mediaDevices?.getUserMedia && typeof window.MediaRecorder !== "undefined";
  const speech = useSpeechToText();
  const { space, workspaceId, viewer } = useSession();
  const scope = { customerId: space?.id ?? viewer.customer_id ?? "", workspaceId: workspaceId ?? "" };
  // Dictation needs speech-to-text and a workspace to bill it to; recording a transcript's audio
  // needs only the microphone.
  const supported = canRecord && (!!onAudio || (speech && !!dictationInput(scope, "", "")));
  const [state, setState] = useState<RecState>("idle");
  const [seconds, setSeconds] = useState(0);
  const [error, setError] = useState<string | null>(null);
  const rec = useRef<{ recorder: MediaRecorder; stream: MediaStream; chunks: Blob[]; bytes: number; format: AudioFormat | null; discard: boolean; timer: number; began: number } | null>(null);
  const handlers = useRef({ onText, onAudio });
  handlers.current = { onText, onAudio };

  const release = () => {
    const r = rec.current;
    if (!r) return;
    window.clearInterval(r.timer);
    r.stream.getTracks().forEach((t) => t.stop());
    rec.current = null;
  };

  useEffect(() => {
    return () => {
      if (rec.current) {
        rec.current.discard = true;
        try {
          rec.current.recorder.stop();
        } catch {
          /* already stopped */
        }
        release();
      }
    };
  }, []);

  const transcribe = async (blob: Blob, format: AudioFormat | null) => {
    setState("transcribing");
    try {
      const audio = await blobToDataUrl(blob);
      const input = dictationInput(scope, `dictation.${extensionFor(blob.type, format)}`, audio);
      if (!input) {
        setError("Choose a workspace first: dictation counts as its speech-to-text usage.");
        return;
      }
      const r = await act<{ text?: string }>("transcribe", input);
      const text = String(r.text ?? "").trim();
      if (text) handlers.current.onText?.(text);
      else setError("Nothing was heard. Try again, closer to the microphone.");
    } catch (e) {
      if (isNotSupported(e)) {
        setError(MICROPHONE_GONE);
        turnSpeechToTextOff();
      } else {
        setError(e instanceof ApiError ? e.message : "The recording could not be transcribed.");
      }
    } finally {
      setState("idle");
    }
  };

  const stop = () => {
    const r = rec.current;
    if (!r) return;
    r.discard = false;
    r.recorder.stop();
  };

  const start = async () => {
    if (!canRecord || state !== "idle") return;
    setError(null);
    let stream: MediaStream;
    try {
      stream = await navigator.mediaDevices.getUserMedia({ audio: true });
    } catch {
      setError("The microphone is blocked. Allow it for this site in the browser's settings; typing and your keyboard's own dictation still work.");
      return;
    }
    const format = pickRecordingFormat((t) => MediaRecorder.isTypeSupported(t));
    const recorder = format ? new MediaRecorder(stream, { mimeType: format.mime }) : new MediaRecorder(stream);
    const chunks: Blob[] = [];
    recorder.ondataavailable = (e) => {
      if (!e.data.size) return;
      chunks.push(e.data);
      if (rec.current) rec.current.bytes += e.data.size;
    };
    recorder.onstop = () => {
      const r = rec.current;
      const discard = r?.discard ?? true;
      const took = r ? (Date.now() - r.began) / 1000 : 0;
      release();
      if (discard) {
        setState("idle");
        return; // cancelled: the recording is dropped, never sent
      }
      const blob = new Blob(chunks, { type: recorder.mimeType || format?.mime || "" });
      if (handlers.current.onAudio) {
        setState("idle");
        handlers.current.onAudio({ blob, name: `recording.${extensionFor(blob.type, format)}`, seconds: took });
        return;
      }
      void transcribe(blob, format);
    };
    const began = Date.now();
    const timer = window.setInterval(() => {
      const s = (Date.now() - began) / 1000;
      setSeconds(s);
      if (recordingFull(rec.current?.bytes ?? 0, s, maxSeconds)) stop();
    }, 250);
    rec.current = { recorder, stream, chunks, bytes: 0, format, discard: false, timer, began };
    setSeconds(0);
    setState("recording");
    recorder.start(1000);
  };

  const cancel = () => {
    const r = rec.current;
    if (!r) return;
    r.discard = true;
    r.recorder.stop();
  };

  return { supported, canRecord, state, seconds, error, start, stop, cancel, clearError: () => setError(null) };
}

export type Recorder = ReturnType<typeof useRecorder>;

/** 44 px on a phone (Apple's minimum for what is tapped often), 32 px from `md` up. */
export const roundButton = "plain inline-flex h-[44px] w-[44px] shrink-0 items-center justify-center rounded-full transition-colors md:h-8 md:w-8 md:rounded-[6px]";

/** The microphone. Hidden where the browser cannot record, and for the session once the lake said
 * it has no speech-to-text; typing still works there. */
export function DictateButton({ rec, title = "Dictate: speak instead of typing", className }: { rec: Recorder; title?: string; className?: string }) {
  if (!rec.supported) return null;
  return (
    <button type="button" onClick={rec.start} disabled={rec.state !== "idle"} aria-label="Dictate" title={title} className={cx(roundButton, "text-ink-2 hover:bg-raised hover:text-ink disabled:opacity-40", className)}>
      {rec.state === "transcribing" ? <Loader2 size={18} className="animate-spin md:h-[15px] md:w-[15px]" /> : <Mic size={18} className="md:h-[15px] md:w-[15px]" />}
    </button>
  );
}

/** While recording: the time, Cancel (discards it) and Stop. */
export function RecordingBar({ rec, stopLabel = "Stop and transcribe", className }: { rec: Recorder; stopLabel?: string; className?: string }) {
  return (
    <div className={cx("flex min-h-[44px] flex-1 items-center gap-2 px-1 md:min-h-8", className)} aria-live="polite">
      <span className="h-2.5 w-2.5 shrink-0 animate-pulse rounded-full bg-bad" aria-hidden />
      <span className="text-[14px] text-ink md:text-[13px]">Recording</span>
      <span className="font-mono text-[13px] text-ink-2">{clock(rec.seconds)}</span>
      <span className="flex-1" />
      <button type="button" onClick={rec.cancel} className="plain h-[44px] rounded-[6px] px-3 text-[14px] text-ink-2 hover:bg-raised md:h-8 md:text-[12.5px]">
        Cancel
      </button>
      <button type="button" onClick={rec.stop} aria-label={stopLabel} title={stopLabel} className={cx(roundButton, "bg-bad text-on-action hover:brightness-110")}>
        <Square size={14} fill="currentColor" />
      </button>
    </div>
  );
}

/** A dictation problem, in words, dismissable. */
export function DictationError({ rec, className }: { rec: Recorder; className?: string }) {
  if (!rec.error) return null;
  return (
    <div role="alert" className={cx("flex items-start gap-2 text-[12.5px] text-bad", className)}>
      <span className="flex-1">{rec.error}</span>
      <button type="button" onClick={rec.clearError} aria-label="Dismiss" className="plain hit rounded text-ink-3 hover:text-ink">
        <X size={13} />
      </button>
    </div>
  );
}
