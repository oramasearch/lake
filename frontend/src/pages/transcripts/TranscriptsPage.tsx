/** Data → Transcripts (`/w/<id>/transcripts?q=`, lake#561, docs/ui/transcripts.md): the workspace's
 * transcripts, newest first, searchable by their text; each opens its own page. New transcript
 * records, uploads or takes pasted text. Phone-first: one column of cards, the search box full
 * width, New transcript a full-screen sheet. */
import { useEffect, useMemo, useState } from "react";
import { useNavigate, useSearchParams } from "react-router-dom";
import { useInfiniteQuery } from "@tanstack/react-query";
import { CalendarClock, FileAudio, Mic, Plus, Search } from "lucide-react";
import { read } from "@/lib/api";
import { date } from "@/lib/format";
import { Banner, Button, Card, Chip, Empty, ErrorBlock, Page, Skeleton, Stat, StatRow, SearchInput, Toolbar } from "@/lib/ui";
import { useToast } from "@/lib/toast";
import { NOT_CONFIGURED, spokenLength } from "@/lib/dictation";
import { excerptOf, sourceLabel, type TranscriptRow } from "@/lib/transcripts";
import { useSpeechToText } from "@/components/dictation/Dictate";
import { WorkspaceGate } from "@/components/WorkspaceGate";
import { qs, useWorkspace, useWsPath } from "@/pages/train/workspace";
import { NewTranscript } from "./NewTranscript";

const PAGE = 50;

type ListAnswer = { transcripts?: TranscriptRow[]; next_cursor?: string | null };

/** `q`, settled for a moment before it is searched. */
function useSettled(value: string, ms = 300): string {
  const [settled, setSettled] = useState(value);
  useEffect(() => {
    const timer = window.setTimeout(() => setSettled(value), ms);
    return () => window.clearTimeout(timer);
  }, [value, ms]);
  return settled;
}

export default function TranscriptsPage() {
  const [params, setParams] = useSearchParams();
  const navigate = useNavigate();
  const toast = useToast();
  const wsPath = useWsPath();
  const { customerId, workspaceId } = useWorkspace();
  const speech = useSpeechToText();
  const [typed, setTyped] = useState(params.get("q") ?? "");
  const q = useSettled(typed.trim());
  const [creating, setCreating] = useState(params.get("new") === "1");

  // The search is the URL, so a search is linkable and Back returns to it.
  useEffect(() => {
    if ((params.get("q") ?? "") === q) return;
    const next = new URLSearchParams(params);
    if (q) next.set("q", q);
    else next.delete("q");
    setParams(next, { replace: true });
  }, [q, params, setParams]);

  const list = useInfiniteQuery({
    queryKey: ["list_transcripts", customerId, workspaceId, q],
    enabled: !!customerId && !!workspaceId,
    initialPageParam: "",
    queryFn: ({ pageParam, signal }) => read<ListAnswer>("list_transcripts", { customer_id: customerId, workspace_id: workspaceId, q, limit: PAGE, cursor: pageParam }, { signal }),
    getNextPageParam: (last) => last.next_cursor || undefined,
  });
  const rows = useMemo(() => list.data?.pages.flatMap((p) => p.transcripts ?? []) ?? [], [list.data]);
  const more = !!list.hasNextPage;
  const recorded = rows.reduce((sum, r) => sum + (r.seconds ?? 0), 0);
  const latest = rows[0]?.created_at;

  const newButton = (
    <Button variant="primary" onClick={() => setCreating(true)}>
      <Plus size={15} aria-hidden /> New transcript
    </Button>
  );

  return (
    <Page eyebrow="Data" title="Transcripts" sub="Conversations, voice memos and calls as text you can search, edit and ask Orama about." actions={newButton}>
      <WorkspaceGate returnTo="/transcripts">
        {!speech && (
          <Banner tone="quiet" title={NOT_CONFIGURED}>
            Recording and audio uploads are off for now. Upload a text file or paste a transcript instead.
          </Banner>
        )}
        <StatRow cols={3}>
          <Stat label="Transcripts" icon={FileAudio} value={list.isLoading ? "…" : `${rows.length}${more ? "+" : ""}`} sub={q ? `matching “${q}”` : "in this workspace"} />
          <Stat label="Recorded" icon={Mic} value={recorded ? spokenLength(recorded) : "—"} sub="of audio, in this list" />
          <Stat label="Latest" icon={CalendarClock} value={latest ? date(latest) : "—"} sub={rows[0]?.title ?? "nothing yet"} />
        </StatRow>
        <Toolbar>
          <SearchInput value={typed} onChange={setTyped} placeholder="Search transcripts" label="Search transcripts" />
        </Toolbar>
        {list.isLoading ? (
          <Skeleton lines={4} />
        ) : list.isError ? (
          <ErrorBlock error={list.error} retry={() => void list.refetch()} what="transcripts" />
        ) : !rows.length ? (
          q ? (
            <Empty icon={Search} title="No matches" action={<Button onClick={() => setTyped("")}>Clear the search</Button>}>
              No transcript mentions “{q}”.
            </Empty>
          ) : (
            <Empty icon={FileAudio} title="No transcripts yet" action={newButton}>
              Record a conversation, upload a voice memo or a text file, or paste a transcript. On a phone, a voice memo saved to Files uploads the same way.
            </Empty>
          )
        ) : (
          <ul className="m-0 grid list-none gap-2 p-0" aria-label="Transcripts">
            {rows.map((row) => (
              <li key={row.id} className="min-w-0">
                <TranscriptCard row={row} to={wsPath(`/transcript?${qs({ id: row.id })}`)} />
              </li>
            ))}
          </ul>
        )}
        {more && (
          <div className="mt-3 flex justify-center">
            <Button onClick={() => void list.fetchNextPage()} loading={list.isFetchingNextPage}>
              Show more
            </Button>
          </div>
        )}
      </WorkspaceGate>
      {creating && (
        <NewTranscript
          onClose={() => setCreating(false)}
          onCreated={(t) => {
            setCreating(false);
            toast("Transcript saved");
            navigate(wsPath(`/transcript?${qs({ id: t.id })}`));
          }}
        />
      )}
    </Page>
  );
}

function TranscriptCard({ row, to }: { row: TranscriptRow; to: string }) {
  const excerpt = excerptOf(row);
  const length = spokenLength(row.seconds);
  return (
    <Card to={to} padded={false}>
      <div className="px-4 py-3">
        <div className="flex min-w-0 flex-wrap items-center gap-x-2 gap-y-1">
          <span className="min-w-0 flex-1 truncate text-[14.5px] font-semibold text-ink">{row.title || "Untitled transcript"}</span>
          <span className="shrink-0 font-mono text-[11px] text-ink-3">{row.created_at ? date(row.created_at) : ""}</span>
        </div>
        {excerpt && <p className="mt-1 mb-0 line-clamp-2 text-[13px] text-ink-2">{excerpt}</p>}
        <div className="mt-2 flex flex-wrap items-center gap-1.5">
          <Chip tone={row.source === "recording" ? "action" : "neutral"}>{sourceLabel(row.source)}</Chip>
          {length && <Chip>{length}</Chip>}
          {row.created_by && <span className="truncate text-[12px] text-ink-3">by {row.created_by}</span>}
        </div>
      </div>
    </Card>
  );
}
