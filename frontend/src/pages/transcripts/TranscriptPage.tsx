// SPDX-License-Identifier: MIT

/** One transcript (`/w/<id>/transcript?id=<id>`, lake#561, docs/ui/transcripts.md): its title and
 * text, with Ask Orama about this (the panel opens with the transcript as its context), Copy, Edit
 * (a `Modal sheet`, so on a phone Save sits above the keyboard) and Delete, which names what it
 * deletes. */
import { useId, useState } from "react";
import { useNavigate, useSearchParams } from "react-router-dom";
import { CalendarClock, Check, Copy, FileText, Pencil, Sparkles, Timer, Type } from "lucide-react";
import { act, ApiError } from "@/lib/api";
import { date, dateTime } from "@/lib/format";
import { Button, Card, Chip, ErrorBlock, Field, Page, Skeleton, Stat, StatRow } from "@/lib/ui";
import { ConfirmButton, Modal, SheetActions } from "@/lib/overlay";
import { useToast } from "@/lib/toast";
import { useTrail } from "@/lib/trail";
import { openAsk } from "@/lib/ask";
import { appendDictation, spokenLength } from "@/lib/dictation";
import { authorLabel, sourceLabel, transcriptAskPage, wordCount, type Transcript } from "@/lib/transcripts";
import { useSession } from "@/lib/session";
import { DictateButton, DictationError, RecordingBar, useRecorder } from "@/components/dictation/Dictate";
import { WorkspaceGate } from "@/components/WorkspaceGate";
import { useSpaceRead, useWorkspace, useWsPath } from "@/pages/train/workspace";

export default function TranscriptPage() {
  const [params] = useSearchParams();
  const id = params.get("id") ?? "";
  const wsPath = useWsPath();
  const q = useSpaceRead<Transcript>(id ? "get_transcript" : null, { id });
  const t = q.data;
  useTrail([{ label: "Transcripts", to: wsPath("/transcripts") }, { label: t?.title || "Transcript" }]);

  return (
    <WorkspaceGate>
      {!id ? (
        <Page title="Transcript">
          <ErrorBlock error={new ApiError(404, "not_found", "No transcript was named. Open one from Transcripts.")} />
        </Page>
      ) : q.isLoading ? (
        <div className="page-container">
          <Skeleton lines={6} />
        </div>
      ) : q.isError || !t ? (
        <Page title="Transcript">
          <ErrorBlock error={q.error} retry={() => void q.refetch()} what="this transcript" />
        </Page>
      ) : (
        <TranscriptView t={t} />
      )}
    </WorkspaceGate>
  );
}

function TranscriptView({ t }: { t: Transcript }) {
  const navigate = useNavigate();
  const toast = useToast();
  const wsPath = useWsPath();
  const { customerId, workspaceId } = useWorkspace();
  const [editing, setEditing] = useState(false);
  const [copied, setCopied] = useState(false);
  const { viewer } = useSession();
  const author = authorLabel(t, viewer.user_id);
  const words = wordCount(t.text ?? "");
  const length = spokenLength(t.seconds);

  const copy = async () => {
    try {
      await navigator.clipboard.writeText(t.text ?? "");
      setCopied(true);
      window.setTimeout(() => setCopied(false), 1600);
    } catch {
      toast("The browser did not allow copying; select the text instead.", "bad");
    }
  };

  const ask = () =>
    openAsk({
      label: `Transcript: ${t.title}`,
      page: transcriptAskPage({ id: t.id, title: t.title, text: t.text ?? "" }),
      draft: `About the transcript “${t.title}”: `,
    });

  return (
    <Page
      eyebrow={
        <>
          Transcript · {sourceLabel(t.source)}
        </>
      }
      title={t.title || "Untitled transcript"}
      sub={
        <>
          {t.created_at ? `Added ${dateTime(t.created_at)}` : ""}
          {author ? ` by ${author}` : ""}
        </>
      }
      actions={
        <>
          <Button variant="primary" onClick={ask}>
            <Sparkles size={14} aria-hidden /> Ask Orama about this
          </Button>
          <Button onClick={() => void copy()} aria-label={copied ? "Copied" : "Copy the text"}>
            {copied ? <Check size={14} aria-hidden /> : <Copy size={14} aria-hidden />} {copied ? "Copied" : "Copy"}
          </Button>
          <Button onClick={() => setEditing(true)}>
            <Pencil size={14} aria-hidden /> Edit
          </Button>
          <ConfirmButton
            variant="danger"
            title={`Delete “${t.title}”?`}
            confirmLabel="Delete transcript"
            confirm={`The transcript “${t.title}” and its text are removed from this workspace. This can't be undone.`}
            onConfirm={async () => {
              await act("delete_transcript", { customer_id: customerId, workspace_id: workspaceId, id: t.id });
              toast("Transcript deleted");
              navigate(wsPath("/transcripts"));
            }}
          >
            Delete
          </ConfirmButton>
        </>
      }
    >
      <StatRow cols={4}>
        <Stat label="Words" icon={Type} value={words.toLocaleString("en-US")} />
        <Stat label="Length" icon={Timer} value={length || "—"} sub={length ? "of audio" : "no recording"} />
        <Stat label="Source" icon={FileText} value={<span className="text-[16px]">{sourceLabel(t.source)}</span>} sub={t.language ? `language ${t.language}` : undefined} />
        <Stat label="Recorded" icon={CalendarClock} value={<span className="text-[16px]">{date(t.recorded_at || t.created_at)}</span>} />
      </StatRow>
      <Card label="Text" actions={t.source === "recording" ? <Chip tone="action">Transcribed</Chip> : undefined}>
        {t.text ? (
          <div className="transcript-text max-w-[46rem] text-[14.5px] leading-[1.7] whitespace-pre-wrap text-ink">{t.text}</div>
        ) : (
          <p className="muted m-0">This transcript has no text.</p>
        )}
      </Card>
      {editing && <EditTranscript t={t} onClose={() => setEditing(false)} />}
    </Page>
  );
}

function EditTranscript({ t, onClose }: { t: Transcript; onClose: () => void }) {
  const toast = useToast();
  const { customerId, workspaceId } = useWorkspace();
  const formId = useId();
  const [title, setTitle] = useState(t.title);
  const [text, setText] = useState(t.text ?? "");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const dictation = useRecorder({ onText: (said) => setText((prev) => appendDictation(prev, said)) });
  const changed = title.trim() !== t.title || text !== (t.text ?? "");

  const save = async () => {
    if (!title.trim() || !changed) return;
    setBusy(true);
    setError(null);
    try {
      await act("update_transcript", { customer_id: customerId, workspace_id: workspaceId, id: t.id, ...(title.trim() !== t.title ? { title: title.trim() } : {}), ...(text !== (t.text ?? "") ? { text } : {}) });
      toast("Transcript saved");
      onClose();
    } catch (e) {
      setError(e instanceof Error ? e.message : "The transcript could not be saved.");
    } finally {
      setBusy(false);
    }
  };

  return (
    <Modal sheet width={760} title="Edit Transcript" onClose={onClose}>
      <form
        id={formId}
        className="grid gap-3"
        onSubmit={(e) => {
          e.preventDefault();
          void save();
        }}
      >
        <Field label="Title" required>
          <input value={title} onChange={(e) => setTitle(e.target.value)} maxLength={200} disabled={busy} />
        </Field>
        <div className="field" data-keep-visible>
          <label htmlFor={`${formId}-text`}>Text</label>
          <textarea id={`${formId}-text`} rows={14} value={text} onChange={(e) => setText(e.target.value)} disabled={busy} />
          <div className="flex items-center gap-2">
            {dictation.state === "recording" ? <RecordingBar rec={dictation} /> : <DictateButton rec={dictation} title="Dictate: speak and it is added to the text" className="border border-rule" />}
            {dictation.state === "transcribing" && <span className="muted text-[12.5px]">Transcribing…</span>}
          </div>
          <DictationError rec={dictation} />
        </div>
        {error && (
          <p className="error m-0" role="alert">
            {error}
          </p>
        )}
        <SheetActions className="flex items-center justify-end gap-2">
          <Button onClick={onClose} disabled={busy}>
            Cancel
          </Button>
          <Button variant="primary" type="submit" form={formId} loading={busy} disabled={!title.trim() || !changed}>
            Save
          </Button>
        </SheetActions>
      </form>
    </Modal>
  );
}
