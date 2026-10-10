/** New transcript (lake#561, docs/ui/transcripts.md): a `Modal sheet`, full screen on a phone with
 * its actions above the keyboard. Four ways in:
 *
 * - Record: the microphone, then a title; `create_transcript` with `audio_base64` (source
 *   "recording"), and lake transcribes it.
 * - Upload audio: a recording from Files or Voice Memos (source "upload"), transcribed the same way.
 * - Upload text: a .txt, .md, .vtt or .srt file read as text (source "upload"); lake cleans the
 *   captions' timings.
 * - Paste: text typed, pasted or dictated (source "text").
 *
 * When lake has no speech-to-text it answers `not_supported`: the two audio ways are turned off for
 * the session, saying why, and the text ways stay. */
import { type ReactNode, useEffect, useId, useMemo, useState } from "react";
import { ChevronLeft, ClipboardPaste, FileAudio, FileText, Loader2, Mic } from "lucide-react";
import { act, ApiError } from "@/lib/api";
import { Banner, Button, Field, cx } from "@/lib/ui";
import { Modal, SheetActions } from "@/lib/overlay";
import { useWorkspace } from "@/pages/train/workspace";
import { AUDIO_ACCEPT, MAX_RECORDING_SECONDS, TEXT_ACCEPT, clock, fileProblem } from "@/lib/phone";
import { NOT_CONFIGURED, appendDictation, isNotSupported, spokenLength, turnSpeechToTextOff } from "@/lib/dictation";
import { recordingTitle, titleFromFile, titleFromText, type Transcript, type TranscriptSource } from "@/lib/transcripts";
import { DictateButton, DictationError, RecordingBar, blobToDataUrl, roundButton, useRecorder, useSpeechToText, type Recording } from "@/components/dictation/Dictate";
import { FromYourPhoneLink, FromYourPhonePanel } from "@/components/phone/FromYourPhone";

type Mode = "choose" | "record" | "audio" | "textfile" | "paste";

const TITLES: Record<Mode, string> = {
  choose: "New Transcript",
  record: "Record",
  audio: "Upload Audio",
  textfile: "Upload Text",
  paste: "Paste a Transcript",
};

/** Seconds since `since`, ticking while `on`. */
function useElapsed(on: boolean): number {
  const [seconds, setSeconds] = useState(0);
  useEffect(() => {
    if (!on) {
      setSeconds(0);
      return;
    }
    const began = Date.now();
    const timer = window.setInterval(() => setSeconds((Date.now() - began) / 1000), 500);
    return () => window.clearInterval(timer);
  }, [on]);
  return seconds;
}

export function NewTranscript({ onClose, onCreated }: { onClose: () => void; onCreated: (t: Transcript) => void }) {
  const { customerId, workspaceId } = useWorkspace();
  const speech = useSpeechToText();
  const [mode, setMode] = useState<Mode>("choose");
  const [title, setTitle] = useState("");
  const [text, setText] = useState("");
  const [file, setFile] = useState<File | null>(null);
  const [recording, setRecording] = useState<(Recording & { at: Date }) | null>(null);
  const [problem, setProblem] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const [phoneHelp, setPhoneHelp] = useState(false);
  // On a phone the actions are portalled out of the form (SheetActions): Save names its form.
  const formId = useId();
  const audioMode = mode === "record" || mode === "audio";
  const elapsed = useElapsed(busy && audioMode);
  const recorder = useRecorder({
    maxSeconds: MAX_RECORDING_SECONDS,
    onAudio: (r) => {
      const at = new Date();
      setRecording({ ...r, at });
      setTitle((t) => t || recordingTitle(at));
    },
  });
  const dictation = useRecorder({ onText: (said) => setText((prev) => appendDictation(prev, said)) });
  const preview = useMemo(() => (recording ? URL.createObjectURL(recording.blob) : ""), [recording]);
  useEffect(() => {
    return () => {
      if (preview) URL.revokeObjectURL(preview);
    };
  }, [preview]);

  const go = (next: Mode) => {
    setMode(next);
    setProblem(null);
    setFile(null);
    setRecording(null);
    setText("");
    setTitle("");
    setPhoneHelp(false);
  };

  const pick = (chosen: File | null, kind: "audio" | "text") => {
    setProblem(null);
    if (!chosen) return;
    const why = fileProblem(chosen, kind);
    if (why) {
      setFile(null);
      setProblem(why);
      return;
    }
    setFile(chosen);
    setTitle((t) => t || titleFromFile(chosen.name));
    if (kind === "text") {
      chosen
        .text()
        .then(setText)
        .catch(() => setProblem("That file could not be read."));
    }
  };

  const ready =
    !busy &&
    (!audioMode || speech) &&
    !!title.trim() &&
    (mode === "record" ? !!recording : mode === "audio" ? !!file : mode === "textfile" ? !!file && !!text.trim() : mode === "paste" ? !!text.trim() : false);

  const create = async () => {
    if (!ready) return;
    setBusy(true);
    setProblem(null);
    try {
      const input: Record<string, unknown> = { customer_id: customerId, workspace_id: workspaceId, title: title.trim() };
      let source: TranscriptSource = "text";
      if (mode === "record" && recording) {
        source = "recording";
        input.audio_base64 = await blobToDataUrl(recording.blob);
        input.audio_name = recording.name;
        input.recorded_at = recording.at.toISOString();
      } else if (mode === "audio" && file) {
        source = "upload";
        input.audio_base64 = await blobToDataUrl(file);
        input.audio_name = file.name;
      } else {
        source = mode === "textfile" ? "upload" : "text";
        input.text = text;
      }
      input.source = source;
      const created = await act<Transcript>("create_transcript", input);
      onCreated(created);
    } catch (e) {
      if (isNotSupported(e)) {
        turnSpeechToTextOff();
        setProblem(null);
      } else {
        setProblem(e instanceof ApiError ? e.message : (e as Error).message || "The transcript could not be saved.");
      }
    } finally {
      setBusy(false);
    }
  };

  const noSpeech = (
    <Banner tone="quiet" title={NOT_CONFIGURED}>
      Recordings and audio files can't be transcribed here. Upload a text file or paste a transcript instead.
    </Banner>
  );

  return (
    <Modal
      sheet
      width={600}
      title={TITLES[mode]}
      subtitle={mode === "choose" ? "Record a conversation, upload a voice memo or a text file, or paste a transcript." : undefined}
      onClose={onClose}
    >
      {mode === "choose" ? (
        <div className="grid gap-3">
          {!speech && noSpeech}
          <div className="grid grid-cols-1 gap-2 sm:grid-cols-2">
            <Choice icon={Mic} title="Record" sub="Use the microphone; lake transcribes it." disabled={!speech || !recorder.canRecord} onClick={() => go("record")} />
            <Choice icon={FileAudio} title="Upload audio" sub="A voice memo or recorded call." disabled={!speech} onClick={() => go("audio")} />
            <Choice icon={FileText} title="Upload text" sub=".txt, .md, .vtt or .srt" onClick={() => go("textfile")} />
            <Choice icon={ClipboardPaste} title="Paste" sub="Type, paste or dictate it." onClick={() => go("paste")} />
          </div>
          {speech && !recorder.canRecord && <p className="muted m-0 text-[12.5px]">This browser can't record here. Upload a recording instead.</p>}
          <div>
            <FromYourPhoneLink open={phoneHelp} onToggle={() => setPhoneHelp((v) => !v)} />
            {phoneHelp && <FromYourPhonePanel where="upload" onClose={() => setPhoneHelp(false)} className="mt-2" />}
          </div>
        </div>
      ) : (
        <form
          id={formId}
          className="grid gap-3"
          onSubmit={(e) => {
            e.preventDefault();
            void create();
          }}
        >
          {audioMode && !speech && noSpeech}

          {mode === "record" && (
            <section aria-label="Recorder" className="rounded-[6px] border border-hairline bg-canvas p-3">
              {recorder.state === "recording" ? (
                <RecordingBar rec={recorder} stopLabel="Stop recording" />
              ) : recording ? (
                <div className="grid gap-2">
                  <div className="flex flex-wrap items-center gap-2 text-[13px] text-ink-2">
                    <span className="font-semibold text-ink">Recorded</span>
                    <span className="font-mono">{clock(recording.seconds)}</span>
                    <span className="flex-1" />
                    <Button variant="ghost" onClick={() => setRecording(null)} disabled={busy}>
                      Record again
                    </Button>
                  </div>
                  {preview && <audio controls src={preview} className="w-full" />}
                </div>
              ) : (
                <div className="flex items-center gap-3">
                  <button
                    type="button"
                    onClick={recorder.start}
                    disabled={!speech || !recorder.canRecord}
                    aria-label="Start recording"
                    className={cx(roundButton, "!h-[52px] !w-[52px] !rounded-full bg-bad text-on-action hover:brightness-110 disabled:opacity-40")}
                  >
                    <Mic size={22} />
                  </button>
                  <div className="min-w-0 text-[13px] text-ink-2">
                    Tap to start. Up to {MAX_RECORDING_SECONDS / 60} minutes or 25 MB; Cancel discards it, Stop keeps it to transcribe.
                  </div>
                </div>
              )}
              <DictationError rec={recorder} className="mt-2" />
            </section>
          )}

          {(mode === "audio" || mode === "textfile") && (
            <Field
              label={mode === "audio" ? "Recording" : "Text file"}
              help={mode === "audio" ? "On a phone: Choose File, then Files (where a saved voice memo is) or Browse." : "WebVTT and SubRip captions lose their timings."}
            >
              <input type="file" accept={mode === "audio" ? AUDIO_ACCEPT : TEXT_ACCEPT} onChange={(e) => pick(e.target.files?.[0] ?? null, mode === "audio" ? "audio" : "text")} disabled={busy} />
            </Field>
          )}
          {mode === "audio" && (
            <div>
              <FromYourPhoneLink open={phoneHelp} onToggle={() => setPhoneHelp((v) => !v)} />
              {phoneHelp && <FromYourPhonePanel where="upload" onClose={() => setPhoneHelp(false)} className="mt-2" />}
            </div>
          )}

          <Field label="Title" required>
            <input value={title} onChange={(e) => setTitle(e.target.value)} placeholder={mode === "record" ? "Weekly sync with the design team" : "Title"} maxLength={200} disabled={busy} />
          </Field>

          {mode === "textfile" && text && (
            <Field label="Preview">
              <pre className="m-0 max-h-48 overflow-auto text-[12.5px] whitespace-pre-wrap">{text.length > 4000 ? `${text.slice(0, 4000)}…` : text}</pre>
            </Field>
          )}

          {mode === "paste" && (
            <div className="field" data-keep-visible>
              <label htmlFor="transcript-text">Text</label>
              <textarea
                id="transcript-text"
                rows={10}
                value={text}
                onChange={(e) => {
                  setText(e.target.value);
                  if (!title) setTitle(titleFromText(e.target.value));
                }}
                placeholder="Paste or type the transcript."
                disabled={busy}
              />
              <div className="flex items-center gap-2">
                {dictation.state === "recording" ? <RecordingBar rec={dictation} /> : <DictateButton rec={dictation} title="Dictate: speak and it is added to the text" className="border border-rule" />}
                {dictation.state === "transcribing" && <span className="muted text-[12.5px]">Transcribing…</span>}
              </div>
              <DictationError rec={dictation} />
            </div>
          )}

          {busy && audioMode && (
            <div role="status" className="flex items-center gap-2 rounded-[6px] bg-action-soft px-3 py-2.5 text-[13px] text-ink">
              <Loader2 size={15} className="shrink-0 animate-spin text-action" />
              <span>
                Uploading and transcribing{elapsed >= 1 ? ` · ${spokenLength(elapsed)}` : ""}. A long recording takes a minute or more; keep this open.
              </span>
            </div>
          )}
          {problem && (
            <p className="error m-0" role="alert">
              {problem}
            </p>
          )}

          <SheetActions className="flex items-center gap-2">
            <Button variant="ghost" onClick={() => go("choose")} disabled={busy}>
              <ChevronLeft size={15} aria-hidden /> Back
            </Button>
            <span className="flex-1" />
            <Button variant="primary" type="submit" loading={busy} disabled={!ready} form={formId}>
              {audioMode ? "Transcribe and save" : "Save transcript"}
            </Button>
          </SheetActions>
        </form>
      )}
    </Modal>
  );
}

function Choice({ icon: IconC, title, sub, onClick, disabled }: { icon: typeof Mic; title: ReactNode; sub: ReactNode; onClick: () => void; disabled?: boolean }) {
  return (
    <button
      type="button"
      onClick={onClick}
      disabled={disabled}
      className="plain flex min-h-[64px] w-full items-center justify-start gap-3 rounded-[6px] border border-hairline bg-surface px-3 py-2.5 text-left transition-colors hover:border-action disabled:opacity-45"
    >
      <span className="flex h-9 w-9 shrink-0 items-center justify-center rounded-full bg-action-soft text-action">
        <IconC size={17} />
      </span>
      <span className="min-w-0">
        <span className="block text-[14px] font-semibold text-ink">{title}</span>
        <span className="block text-[12.5px] text-ink-2">{sub}</span>
      </span>
    </button>
  );
}
