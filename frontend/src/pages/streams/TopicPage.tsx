/** One topic (web_ui_streams.cpp, topic mode; its `#streams-controls` script): settings and
 * subscriptions, the retained messages newest first with paging and a jump, Live (the bounded
 * forward long poll of `browse_stream`), and the confirmed writes: edit settings, publish, edit or
 * delete a message, purge and drop. */
import { type FormEvent, useEffect, useMemo, useState } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { act, read, useRead, type Row } from "@/lib/api";
import { useSession } from "@/lib/session";
import { Badge, ErrorBlock, Page, Skeleton } from "@/lib/ui";
import { formValues } from "@/pages/apps/AppsPage";
import { errorText, PageError, pause, qs, useRunner, useSpacePermissions, useWorkspaceLink } from "@/pages/apps/shared";
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

  if (!topic) {
    return (
      <Page eyebrow="Train" title="Streams" sub={STREAMS_SUB}>
        <p>
          <Link to={ws("/train/streams") + qs({ customer_id: customerId })}>All streams</Link>
        </p>
      </Page>
    );
  }
  if (detail.isLoading || messages.isLoading) {
    return (
      <Page eyebrow="Train" title="Streams" sub={STREAMS_SUB}>
        <Skeleton />
      </Page>
    );
  }
  const failed = detail.isError ? detail : messages.isError ? messages : null;
  if (failed) {
    return (
      <Page eyebrow="Train" title="Streams" sub={STREAMS_SUB}>
        <p>
          <Link to={ws("/train/streams") + qs({ customer_id: customerId })}>All streams</Link>
        </p>
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
    );
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
    );
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
  return (
    <Page eyebrow="Train" title="Streams" sub={STREAMS_SUB}>
      <PageError error={runner.error ?? liveError} />
      {confirmDialog}
      <p>
        <Link to={ws("/train/streams") + qs({ customer_id: customerId })}>All streams</Link>
      </p>
      <h2>{displayName(topic)}</h2>
      {system && (
        <p>
          <Badge>system · browse-only</Badge>
        </p>
      )}
      <details>
        <summary>Settings</summary>
        <pre>{JSON.stringify(settings, null, 2)}</pre>
      </details>

      <h2>Subscriptions</h2>
      <div className="table-wrap">
        <table>
          <thead>
            <tr>
              <th>Subscriber</th>
              <th>Position</th>
              <th>Next Sequence</th>
              <th>Lag</th>
            </tr>
          </thead>
          <tbody>
            {(info.subscriptions ?? []).map((subscription, index) => (
              <tr key={index}>
                <td>{String(subscription.subscriber ?? "")}</td>
                <td>{JSON.stringify(subscription.positions)}</td>
                <td>{String(subscription.next_sequence ?? "")}</td>
                <td>{String(subscription.lag ?? "")}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>

      {zetta === "workspace" && topic === "agent-events" && (
        <form className="filters" onSubmit={filterEvents}>
          <label>
            Turn (task ID)
            <input name="task" defaultValue={task} key={`task-${task}`} />
          </label>
          <label>
            Agent
            <input name="agent" defaultValue={agent} key={`agent-${agent}`} />
          </label>
          <button type="submit">Filter events</button>
        </form>
      )}

      {!system && manage && canWrite && (
        <details>
          <summary>Edit Settings</summary>
          <form className="stack" onSubmit={updateSettings}>
            <label>
              Mutable settings (JSON)
              <textarea name="settings" rows={12} required defaultValue={JSON.stringify(mutableSettings, null, 2)} key={JSON.stringify(mutableSettings)} />
            </label>
            <p>A retention shrink removes old messages immediately. The confirmation shows the current trim count.</p>
            <button type="submit" disabled={runner.busy === "settings"}>
              Preview and save settings
            </button>
          </form>
        </details>
      )}

      <h2>Messages</h2>
      <form className="filters" onSubmit={jump}>
        <label>
          Jump to sequence
          <input type="number" min={1} name="from_sequence" required />
        </label>
        <button type="submit">Jump</button>
      </form>
      <label>
        <input type="checkbox" checked={live} onChange={(e) => setLive(e.target.checked)} /> Live
      </label>
      <p>Uncheck Live to pause. Copy and edit use the displayed revision. Refresh to see edits to older messages.</p>
      <div id="stream-messages">
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
          />
        ))}
      </div>
      {nextPage !== null && nextPage !== undefined && <Link to={`${base}&from_sequence=${nextPage}`}>Older messages</Link>}
      <p>
        <Link to={base}>Newest messages</Link>
      </p>

      {!system && canWrite && (
        <>
          <h2>Publish Message</h2>
          <form className="stack" onSubmit={publish}>
            <label>
              JSON payload
              <textarea name="payload" required defaultValue="{}" />
            </label>
            <button type="submit" disabled={runner.busy === "publish"}>
              Publish
            </button>
          </form>
        </>
      )}
      {!system && canAdmin && (
        <>
          <h2>Purge Messages</h2>
          <form className="stack" onSubmit={purge}>
            <label>
              Before sequence (exclusive)
              <input name="before_sequence" type="number" min={1} required />
            </label>
            <p>Subscribers that have not received these messages will skip them.</p>
            <button type="submit" disabled={runner.busy === "purge"}>
              Preview purge
            </button>
          </form>
          {manage && (
            <>
              <h2>Delete Topic</h2>
              <form className="stack" onSubmit={drop}>
                <label>
                  Type the topic name
                  <input name="confirm_name" required />
                </label>
                <p>This removes all held messages and closes subscriptions.</p>
                <button className="danger" type="submit" disabled={runner.busy === "drop"}>
                  Delete topic
                </button>
              </form>
            </>
          )}
        </>
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
}: {
  event: StreamEvent;
  writable: boolean;
  canWrite: boolean;
  canAdmin: boolean;
  busy: string | null;
  onSave: (payload: string, redeliver: boolean) => Promise<unknown>;
  onDelete: () => Promise<unknown>;
  onCopyError: (error: unknown) => void;
}) {
  const payload = pretty(event.payload ?? "");
  const [copied, setCopied] = useState(false);
  return (
    <article className="panel" data-sequence={event.sequence}>
      <h3>Message {event.sequence}</h3>
      <p>
        Sender: {event.from ?? ""} · Published: {String(event.published_at ?? "")} (Unix ms) · Size: {String(event.size ?? "")} bytes · Revision: {String(event.revision ?? "")}
      </p>
      <details open={payload.length <= 1024}>
        <summary>JSON Payload</summary>
        <pre>{payload}</pre>
      </details>
      <button
        type="button"
        onClick={async () => {
          try {
            await navigator.clipboard.writeText(payload);
            setCopied(true);
          } catch (e) {
            onCopyError(new Error(errorText(e) || "Copy failed."));
          }
        }}
      >
        {copied ? "Copied" : "Copy message"}
      </button>
      {writable && canWrite && (
        <details>
          <summary>Edit Payload</summary>
          <form
            className="stack"
            onSubmit={(e) => {
              e.preventDefault();
              const form = new FormData(e.currentTarget);
              void onSave(String(form.get("payload") ?? ""), form.get("redeliver") === "on");
            }}
          >
            <label>
              JSON payload
              <textarea name="payload" required defaultValue={payload} />
            </label>
            <label>
              <input type="checkbox" name="redeliver" /> Re-deliver to subscribers that already received it
            </label>
            <p>
              Off by default. Enabling this queues the edited payload again for subscriptions that received this message, including those that acknowledged it. Pending subscribers receive the
              edit either way.
            </p>
            <button type="submit" disabled={busy === `edit-${event.sequence}`}>
              Save payload
            </button>
          </form>
        </details>
      )}
      {writable && canAdmin && (
        <button className="danger" type="button" disabled={busy === `delete-${event.sequence}`} onClick={() => void onDelete()}>
          Delete message
        </button>
      )}
    </article>
  );
}
