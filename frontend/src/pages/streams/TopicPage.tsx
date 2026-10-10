/** One topic (web_ui_streams.cpp, topic mode; its `#streams-controls` script), as a message
 * browser: the retained messages newest first as rows, with Live (the bounded forward long poll of
 * `browse_stream`) as the prominent control, paging and a jump; a side column with the topic,
 * its settings and subscriptions; and the confirmed writes: edit settings, publish, edit or delete
 * a message, purge and drop. */
import { type FormEvent, useEffect, useMemo, useState } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { ArrowLeft, Check, Copy, Pencil, Send, Trash2 } from "lucide-react";
import { act, read, useRead, type Row } from "@/lib/api";
import { bytes, relative } from "@/lib/format";
import { Drawer, Modal } from "@/lib/overlay";
import { useSession } from "@/lib/session";
import { Banner, Button, Card, Chip, Empty, ErrorBlock, FormError, KeyValue, Page, Skeleton, Split } from "@/lib/ui";
import { formValues } from "@/pages/apps/AppsPage";
import { errorText, LiveSwitch, PageError, pause, qs, useRunner, useSpacePermissions, useWorkspaceLink } from "@/pages/apps/shared";
import { displayName, pretty, STREAMS_SUB, useConfirm, useStreamsTrail } from "./common";

type StreamEvent = { sequence: number; revision?: number; from?: string; published_at?: number; size?: number; payload?: unknown };
type Detail = { system?: boolean; can_manage?: boolean; settings?: Row; subscriptions?: Row[] };
type Messages = { events?: StreamEvent[]; next_sequence?: number; next_page_sequence?: number | null };

/** The structural settings an update cannot change (web_ui_streams.cpp). */
const STRUCTURAL = ["lifecycle", "store", "ring_capacity", "inline_threshold", "priority_levels", "producer_queue", "exposed"];

function parseJson(text: string, field: string): unknown {
  try {
    return JSON.parse(text);
  } catch (e) {
    throw new Error(`${field}: ${(e as Error).message}`);
  }
}

/** A different topic, filter or jump is a different page: its live state starts afresh. */
export default function TopicPage() {
  const [params] = useSearchParams();
  return <TopicView key={params.toString()} />;
}

function TopicView() {
  useStreamsTrail();
  const { space, workspaceId } = useSession();
  const ws = useWorkspaceLink();
  const navigate = useNavigate();
  const [params] = useSearchParams();
  const customerId = space?.id ?? "";
  const zetta = params.get("zetta") ?? "";
  const topic = params.get("topic") ?? "";
  const task = params.get("task") ?? "";
  const agent = params.get("agent") ?? "";
  const fromText = params.get("from_sequence") ?? "";
  const permissions = useSpacePermissions(customerId);
  const runner = useRunner();
  const [confirmDialog, confirm] = useConfirm();
  const [publishing, setPublishing] = useState(false);
  const [editing, setEditing] = useState(false);

  const scope = { customer_id: customerId, workspace_id: workspaceId ?? undefined, zetta, topic };
  const detail = useRead<Detail>(customerId && topic ? "get_stream" : null, scope);
  const messages = useRead<Messages>(customerId && topic ? "browse_stream" : null, {
    ...scope,
    from_sequence: /^\d+$/.test(fromText) ? Number(fromText) : fromText || undefined,
    direction: "backward",
    task: task || undefined,
    agent: agent || undefined,
  });

  // Live: forward reads from the allocator's high-water mark, new messages first, at most 200.
  const [live, setLive] = useState(false);
  const [liveEvents, setLiveEvents] = useState<StreamEvent[]>([]);
  const [liveError, setLiveError] = useState<unknown>(null);
  const startCursor = messages.data?.next_sequence ?? 1;
  useEffect(() => {
    if (!live) return;
    const controller = new AbortController();
    const signal = controller.signal;
    let cursor = startCursor;
    setLiveError(null);
    void (async () => {
      try {
        while (!signal.aborted) {
          const page = await read<Messages>(
            "browse_stream",
            { ...scope, from_sequence: cursor, direction: "forward", limit: 100, wait_ms: 10000, task: task || undefined, agent: agent || undefined },
            { signal },
          );
          if (signal.aborted) break;
          const events = page.events ?? [];
          if (events.length) setLiveEvents((current) => [...[...events].reverse(), ...current].slice(0, 200));
          cursor = page.next_page_sequence ?? page.next_sequence ?? cursor;
          await pause(250, signal);
        }
      } catch (e) {
        if (!signal.aborted && (e as Error).name !== "AbortError") {
          setLiveError(e);
          setLive(false);
        }
      }
    })();
    return () => {
      controller.abort();
    };
    // The cursor and scope are read when Live is switched on; another topic is another page.
  }, [live]);

  const shown = useMemo(() => {
    const seen = new Set<number>();
    const out: StreamEvent[] = [];
    for (const event of [...liveEvents, ...(messages.data?.events ?? [])]) {
      if (seen.has(event.sequence)) continue;
      seen.add(event.sequence);
      out.push(event);
    }
    return out.slice(0, 200);
  }, [liveEvents, messages.data]);

  const base = ws("/train/stream") + qs({ customer_id: customerId, zetta, topic, task, agent });

  const allStreams = (
    <Button to={ws("/train/streams") + qs({ customer_id: customerId })}>
      <ArrowLeft size={14} aria-hidden /> All streams
    </Button>
  );
  if (!topic) {
    return (
      <Page eyebrow="Train" title="Streams" sub={STREAMS_SUB} actions={allStreams}>
        <Empty title="No Topic Named">Open a topic from the list of streams.</Empty>
      </Page>
    );
  }
  if (detail.isLoading || messages.isLoading) {
    return (
      <Page eyebrow="Train · Streams" title={displayName(topic)} sub={STREAMS_SUB} actions={allStreams}>
        <Skeleton />
      </Page>
    );
  }
  const failed = detail.isError ? detail : messages.isError ? messages : null;
  if (failed) {
    return (
      <Page eyebrow="Train · Streams" title={displayName(topic)} sub={STREAMS_SUB} actions={allStreams}>
        <ErrorBlock error={failed.error} retry={() => void failed.refetch()} what="this topic" />
      </Page>
    );
  }

  const info = detail.data ?? {};
  const system = info.system !== false;
  const manage = !!info.can_manage;
  const settings = (info.settings ?? {}) as Row;
  const mutableSettings = Object.fromEntries(Object.entries(settings).filter(([key]) => !STRUCTURAL.includes(key)));
  const { canWrite, canAdmin } = permissions;

  /** A write, its toast only when it went ahead (a cancelled confirmation answers undefined). */
  const write = (key: string, action: () => Promise<unknown>, done?: string) => runner.run(key, action, (value) => (value === undefined ? undefined : done));

  const updateSettings = (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    const form = formValues(event);
    void write(
      "settings",
      async () => {
        const patch = parseJson(form.settings ?? "", "Settings");
        const preview = await act<Row>("update_stream", { ...scope, settings: patch, dry_run: true });
        if (!(await confirm(`Apply these settings? This currently trims ${preview.trimmed} held messages. Messages published or expired meanwhile may change the count.`, "Apply"))) return undefined;
        return act("update_stream", { ...scope, settings: patch });
      },
      "Settings saved",
    ).then((value) => {
      if (value !== undefined) setEditing(false);
    });
  };

  const deleteMessages = (key: string, selector: Row) =>
    write(
      key,
      async () => {
        const preview = await act<Row>("delete_stream_messages", { ...scope, ...selector, dry_run: true });
        if (!(await confirm(`Delete ${preview.removed} held messages? Subscribers that have not received them will skip them.`, "Delete"))) return undefined;
        return act("delete_stream_messages", { ...scope, ...selector, confirm_count: preview.removed });
      },
      "Messages deleted",
    );

  const publish = (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    const element = event.currentTarget;
    const form = formValues(event);
    void write(
      "publish",
      async () => {
        parseJson(form.payload ?? "", "Payload");
        const result = await act<Row>("publish_stream_message", { ...scope, payload: form.payload });
        if (!result.accepted) throw new Error("Zetta did not accept the message. Check the topic limits and overflow policy.");
        element.reset();
        return result;
      },
      "Message published",
    ).then((value) => {
      if (value !== undefined) setPublishing(false);
    });
  };

  const purge = (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    const form = formValues(event);
    void deleteMessages("purge", { before_sequence: Number(form.before_sequence) });
  };

  const drop = (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    const form = formValues(event);
    void write("drop", async () => {
      const name = form.confirm_name ?? "";
      if (name !== topic && `topic:${name}` !== topic) throw new Error("Type the exact topic name to confirm deletion.");
      await act("delete_stream", { ...scope, confirm_name: name });
      navigate(ws("/train/streams") + qs({ customer_id: customerId }));
    });
  };

  const jump = (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    const form = formValues(event);
    navigate(base + `&from_sequence=${encodeURIComponent(form.from_sequence ?? "")}`);
  };

  const filterEvents = (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    const form = formValues(event);
    navigate(ws("/train/stream") + qs({ customer_id: customerId, zetta, topic, task: form.task, agent: form.agent }));
  };

  const nextPage = messages.data?.next_page_sequence;
  const subscriptions = info.subscriptions ?? [];
  const closeOverlay = () => {
    setPublishing(false);
    setEditing(false);
    runner.setError(null);
  };
  const overlayOpen = publishing || editing;
  return (
    <Page
      eyebrow="Train · Streams"
      title={
        <span className="inline-flex flex-wrap items-center gap-2.5">
          {displayName(topic)} {system && <Chip>system · browse-only</Chip>}
        </span>
      }
      sub={STREAMS_SUB}
      actions={
        <>
          {allStreams}
          {!system && canWrite && (
            <Button variant="primary" onClick={() => setPublishing(true)}>
              <Send size={14} aria-hidden /> Publish message
            </Button>
          )}
        </>
      }
    >
      {!overlayOpen && <PageError error={runner.error ?? liveError} />}
      {confirmDialog}
      <Split
        side={
          <>
            <Card label="Topic" title={<span className="break-all font-mono text-[13px]">{topic}</span>}>
              <KeyValue
                rows={[
                  ["Zetta", zetta === "bus" ? "Lake bus" : "Workspace Zetta"],
                  ["Kind", system ? "System (browse-only)" : "Topic"],
                  ["Next sequence", <span key="n" className="font-mono">{String(messages.data?.next_sequence ?? "")}</span>],
                  ["Subscribers", <span key="s" className="font-mono">{subscriptions.length}</span>],
                ]}
              />
            </Card>
            <Card
              label="Settings"
              title="Topic Settings"
              actions={
                !system && manage && canWrite ? (
                  <Button onClick={() => setEditing(true)}>
                    <Pencil size={14} aria-hidden /> Edit
                  </Button>
                ) : undefined
              }
            >
              <details>
                <summary className="cursor-pointer text-[13px] text-ink-2">Settings</summary>
                <pre className="mt-2">{JSON.stringify(settings, null, 2)}</pre>
              </details>
            </Card>
            <Card label="Subscriptions" title="Subscribers" padded={false}>
              {subscriptions.length === 0 ? (
                <p className="muted m-0 px-4 py-3 text-[13px]">No subscriptions.</p>
              ) : (
                <ul className="m-0 list-none p-0">
                  {subscriptions.map((subscription, index) => (
                    <li key={index} className="border-b border-hairline px-4 py-2.5 last:border-b-0">
                      <div className="flex items-center justify-between gap-2">
                        <span className="min-w-0 truncate font-semibold text-ink">{String(subscription.subscriber ?? "")}</span>
                        <span className="font-mono text-[12px] text-ink-2" title="Lag">
                          lag {String(subscription.lag ?? "")}
                        </span>
                      </div>
                      <div className="mt-0.5 truncate font-mono text-[11.5px] text-ink-3" title={JSON.stringify(subscription.positions)}>
                        next {String(subscription.next_sequence ?? "")} · position {JSON.stringify(subscription.positions)}
                      </div>
                    </li>
                  ))}
                </ul>
              )}
            </Card>
            {!system && canAdmin && (
              <Card label="Danger zone" title="Purge and Delete" tone="exception">
                <h3 className="mt-0 text-[13px]">Purge Messages</h3>
                <form className="row-form flex flex-col gap-3" onSubmit={purge}>
                  <label>
                    Before sequence (exclusive)
                    <input name="before_sequence" type="number" min={1} required />
                  </label>
                  <p className="muted m-0 text-[12.5px]">Subscribers that have not received these messages will skip them.</p>
                  <div>
                    <Button type="submit" loading={runner.busy === "purge"}>
                      Preview purge
                    </Button>
                  </div>
                </form>
                {manage && (
                  <>
                    <h3 className="mt-4 border-t border-hairline pt-3 text-[13px]">Delete Topic</h3>
                    <form className="row-form flex flex-col gap-3" onSubmit={drop}>
                      <label>
                        Type the topic name
                        <input name="confirm_name" required />
                      </label>
                      <p className="muted m-0 text-[12.5px]">This removes all held messages and closes subscriptions.</p>
                      <div>
                        <Button variant="danger" type="submit" loading={runner.busy === "drop"}>
                          <Trash2 size={14} aria-hidden /> Delete topic
                        </Button>
                      </div>
                    </form>
                  </>
                )}
              </Card>
            )}
          </>
        }
      >
        <Card
          title="Messages"
          label={`${shown.length} shown · newest first`}
          padded={false}
          actions={<LiveSwitch live={live} onChange={setLive} />}
        >
          <div className="flex flex-wrap items-end gap-2 border-b border-hairline px-4 py-3">
            <form className="row-form flex flex-wrap items-end gap-1.5" onSubmit={jump}>
              <label className="text-[12.5px]">
                Jump to sequence
                <input type="number" min={1} name="from_sequence" required className="w-32" />
              </label>
              <Button type="submit">Jump</Button>
            </form>
            {zetta === "workspace" && topic === "agent-events" && (
              <form className="row-form flex flex-wrap items-end gap-1.5" onSubmit={filterEvents}>
                <label className="text-[12.5px]">
                  Turn (task ID)
                  <input name="task" defaultValue={task} key={`task-${task}`} />
                </label>
                <label className="text-[12.5px]">
                  Agent
                  <input name="agent" defaultValue={agent} key={`agent-${agent}`} className="w-36" />
                </label>
                <Button type="submit">Filter events</Button>
              </form>
            )}
            <Button variant="ghost" to={base} className="ml-auto">
              Newest messages
            </Button>
          </div>
          <p className="muted m-0 border-b border-hairline px-4 py-2 text-[12px]">
            {live ? "Following new messages. Pause Live to stop." : "Paused. Turn on Live to follow new messages."} Copy and edit use the displayed revision. Refresh to see edits to older messages.
          </p>
          <div id="stream-messages">
            {shown.length === 0 && <p className="muted m-0 px-4 py-6 text-center text-[13px]">No retained messages{live ? " yet. New ones appear here as they are published." : "."}</p>}
            {shown.map((event) => (
              <MessageCard
                key={`${event.sequence}-${event.revision}`}
                event={event}
                writable={!system}
                canWrite={canWrite}
                canAdmin={canAdmin}
                busy={runner.busy}
                onSave={(payload, redeliver) =>
                  write(
                    `edit-${event.sequence}`,
                    async () => {
                      parseJson(payload, "Payload");
                      return act("update_stream_message", { ...scope, sequence: event.sequence, expected_revision: event.revision, payload, redeliver });
                    },
                    `Message ${event.sequence} saved`,
                  )
                }
                onDelete={() => deleteMessages(`delete-${event.sequence}`, { sequences: [event.sequence] })}
                onCopyError={(e) => runner.setError(e)}
                error={runner.error}
              />
            ))}
          </div>
          {nextPage !== null && nextPage !== undefined && (
            <div className="border-t border-hairline px-4 py-2.5">
              <Link to={`${base}&from_sequence=${nextPage}`}>Older messages</Link>
            </div>
          )}
        </Card>
      </Split>

      {publishing && !system && canWrite && (
        <Modal
          title="Publish Message"
          subtitle={displayName(topic)}
          onClose={closeOverlay}
          footer={
            <>
              <Button onClick={closeOverlay}>Cancel</Button>
              <Button variant="primary" type="submit" form="publish-message" loading={runner.busy === "publish"}>
                <Send size={14} aria-hidden /> Publish
              </Button>
            </>
          }
        >
          <FormError error={runner.error} />
          <form id="publish-message" className="stack" onSubmit={publish}>
            <label>
              JSON payload
              <textarea name="payload" rows={8} required defaultValue="{}" className="font-mono" />
            </label>
          </form>
        </Modal>
      )}

      {editing && !system && manage && canWrite && (
        <Drawer
          title="Edit Settings"
          subtitle={displayName(topic)}
          onClose={closeOverlay}
          footer={
            <>
              <Button onClick={closeOverlay}>Cancel</Button>
              <Button variant="primary" type="submit" form="topic-settings" loading={runner.busy === "settings"}>
                Preview and save settings
              </Button>
            </>
          }
        >
          <FormError error={runner.error} />
          <Banner tone="quiet">A retention shrink removes old messages immediately. The confirmation shows the current trim count.</Banner>
          <form id="topic-settings" className="stack" onSubmit={updateSettings}>
            <label>
              Mutable settings (JSON)
              <textarea name="settings" rows={14} required defaultValue={JSON.stringify(mutableSettings, null, 2)} key={JSON.stringify(mutableSettings)} className="font-mono" />
            </label>
          </form>
        </Drawer>
      )}
    </Page>
  );
}

function MessageCard({
  event,
  writable,
  canWrite,
  canAdmin,
  busy,
  onSave,
  onDelete,
  onCopyError,
  error,
}: {
  event: StreamEvent;
  writable: boolean;
  canWrite: boolean;
  canAdmin: boolean;
  busy: string | null;
  onSave: (payload: string, redeliver: boolean) => Promise<unknown>;
  onDelete: () => Promise<unknown>;
  onCopyError: (error: unknown) => void;
  error: unknown;
}) {
  const payload = pretty(event.payload ?? "");
  const [copied, setCopied] = useState(false);
  const [editing, setEditing] = useState(false);
  const published = event.published_at;
  return (
    <article className="border-b border-hairline px-4 py-3 last:border-b-0" data-sequence={event.sequence}>
      <div className="flex flex-wrap items-start justify-between gap-2">
        <div className="min-w-0">
          <h3 className="m-0 text-[13.5px] font-semibold text-ink">
            Message <span className="font-mono">{event.sequence}</span>
          </h3>
          <div className="mt-0.5 flex flex-wrap gap-x-3 gap-y-0.5 text-[12px] text-ink-3">
            <span>Sender: {event.from ?? ""}</span>
            <span title={`${String(published ?? "")} (Unix ms)`}>Published: {published !== undefined ? relative(published, Date.now(), String(published)) : ""}</span>
            <span className="font-mono">{event.size !== undefined ? bytes(event.size) : ""}</span>
            <span className="font-mono">rev {String(event.revision ?? "")}</span>
          </div>
        </div>
        <div className="flex min-w-0 flex-wrap gap-1.5">
          <Button
            variant="ghost"
            onClick={async () => {
              try {
                await navigator.clipboard.writeText(payload);
                setCopied(true);
              } catch (e) {
                onCopyError(new Error(errorText(e) || "Copy failed."));
              }
            }}
          >
            {copied ? <Check size={14} aria-hidden /> : <Copy size={14} aria-hidden />} {copied ? "Copied" : "Copy message"}
          </Button>
          {writable && canWrite && (
            <Button variant="ghost" onClick={() => setEditing(true)}>
              <Pencil size={14} aria-hidden /> Edit
            </Button>
          )}
          {writable && canAdmin && (
            <Button variant="ghost" loading={busy === `delete-${event.sequence}`} onClick={() => void onDelete()}>
              <Trash2 size={14} aria-hidden /> Delete message
            </Button>
          )}
        </div>
      </div>
      <details open={payload.length <= 1024} className="mt-2">
        <summary className="cursor-pointer text-[12px] text-ink-2">JSON payload</summary>
        <pre className="mb-0 mt-1.5 max-h-80 overflow-auto">{payload}</pre>
      </details>
      {editing && (
        <Modal
          title={`Edit Message ${event.sequence}`}
          onClose={() => setEditing(false)}
          footer={
            <>
              <Button onClick={() => setEditing(false)}>Cancel</Button>
              <Button variant="primary" type="submit" form={`edit-message-${event.sequence}`} loading={busy === `edit-${event.sequence}`}>
                Save payload
              </Button>
            </>
          }
        >
          <FormError error={error} />
          <form
            id={`edit-message-${event.sequence}`}
            className="stack"
            onSubmit={(e) => {
              e.preventDefault();
              const form = new FormData(e.currentTarget);
              void onSave(String(form.get("payload") ?? ""), form.get("redeliver") === "on").then((value) => {
                if (value !== undefined) setEditing(false);
              });
            }}
          >
            <label>
              JSON payload
              <textarea name="payload" rows={10} required defaultValue={payload} className="font-mono" />
            </label>
            <label>
              <input type="checkbox" name="redeliver" /> Re-deliver to subscribers that already received it
            </label>
            <p className="muted m-0 text-[12.5px]">
              Off by default. Enabling this queues the edited payload again for subscriptions that received this message, including those that acknowledged it. Pending subscribers receive the
              edit either way.
            </p>
          </form>
        </Modal>
      )}
    </article>
  );
}
