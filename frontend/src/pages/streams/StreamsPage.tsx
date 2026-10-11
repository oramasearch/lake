/** Train → Streams (web_ui_streams.cpp, list mode; serve_streams_page): the workspace Zetta's and
 * the Lake bus's topics with their figures, paged and filtered by name prefix, and New topic in a
 * drawer for those who may write. */
import { type FormEvent, useState } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { Inbox, Plus, Radio, Users } from "lucide-react";
import { act, useRead, type Row } from "@/lib/api";
import { num } from "@/lib/format";
import { Drawer } from "@/lib/overlay";
import { useSession } from "@/lib/session";
import { Banner, Button, Card, Chip, type Column, DataTable, Empty, ErrorBlock, FormError, Page, Skeleton, Stat, StatRow, Toolbar } from "@/lib/ui";
import { formValues } from "@/pages/apps/AppsPage";
import { PageError, qs, useRunner, useSpacePermissions, useWorkspaceLink } from "@/pages/apps/shared";
import { displayName, STREAMS_SUB, useStreamsTrail } from "./common";

type Listing = {
  groups?: { zetta: string; topics?: Row[]; error?: string }[];
  next_cursor?: string | null;
  previous_cursor?: string | null;
};

const DEFAULT_SETTINGS = `{
  "retention_ms": 86400000,
  "max_events": 1000,
  "delivery": {"guarantee": "at_least_once"}
}`;

/** The page size the server accepted: an integer from 1 to 200, else 50. */
function pageSize(value: string | null): number {
  if (!value || !/^\d+$/.test(value)) return 50;
  const n = Number(value);
  return n >= 1 && n <= 200 ? n : 50;
}

export default function StreamsPage() {
  useStreamsTrail();
  const { space, workspaceId } = useSession();
  const ws = useWorkspaceLink();
  const navigate = useNavigate();
  const [params] = useSearchParams();
  const customerId = space?.id ?? "";
  const prefix = params.get("prefix") ?? "";
  const limit = pageSize(params.get("limit"));
  const cursor = params.get("cursor") ?? "";
  const permissions = useSpacePermissions(customerId);
  const runner = useRunner();
  const [creating, setCreating] = useState(false);
  const listing = useRead<Listing>(customerId ? "list_streams" : null, {
    customer_id: customerId,
    workspace_id: workspaceId ?? undefined,
    prefix,
    limit,
    cursor: cursor || undefined,
  });

  const pageQuery = (extra: Record<string, string | null | undefined>) => ws("/train/streams") + qs({ customer_id: customerId, prefix, limit, ...extra });

  const filter = (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    const form = formValues(event);
    navigate(ws("/train/streams") + qs({ customer_id: customerId, prefix: form.prefix, limit: form.limit }));
  };

  const create = (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    const form = formValues(event);
    void runner.run("create", async () => {
      let settings: unknown;
      try {
        settings = JSON.parse(form.settings ?? "");
      } catch (e) {
        throw new Error(`Settings: ${(e as Error).message}`);
      }
      const created = await act<Row>("create_stream", { customer_id: customerId, workspace_id: workspaceId, zetta: "workspace", topic: form.topic, settings });
      navigate(ws("/train/stream") + qs({ customer_id: customerId, zetta: "workspace", topic: String(created.topic ?? form.topic) }));
    });
  };

  const groups = listing.data?.groups ?? [];
  const topics = groups.flatMap((group) => group.topics ?? []);
  const counted = topics.filter((topic) => topic.counts_available !== false);
  const held = counted.reduce((sum, topic) => sum + (Number(topic.held_events) || 0), 0);
  const subscribers = counted.reduce((sum, topic) => sum + (Number(topic.subscribers) || 0), 0);

  const columns = (zetta: string): Column<Row>[] => [
    {
      key: "topic",
      label: "Topic",
      sort: (topic) => String(topic.display_name ?? displayName(String(topic.topic ?? ""))),
      render: (topic) => {
        const name = String(topic.topic ?? "");
        const label = String(topic.display_name ?? displayName(name));
        return (
          <div className="min-w-0">
            <div className="flex flex-wrap items-center gap-1.5">
              <Link to={ws("/train/stream") + qs({ customer_id: customerId, zetta, topic: name })} className="font-semibold">
                {label}
              </Link>
              {topic.system ? <Chip>system</Chip> : null}
            </div>
            {label !== name && <div className="truncate font-mono text-[11.5px] text-ink-3">{name}</div>}
          </div>
        );
      },
    },
    { key: "subscribers", label: "Subscribers", numeric: true, sort: (topic) => Number(topic.subscribers) || 0, render: (topic) => <span className="font-mono">{topic.counts_available !== false ? String(topic.subscribers ?? "") : "—"}</span> },
    { key: "held", label: "Held Messages", numeric: true, sort: (topic) => Number(topic.held_events) || 0, render: (topic) => <span className="font-mono">{topic.counts_available !== false ? String(topic.held_events ?? "") : "—"}</span> },
    { key: "next", label: "Next Sequence", numeric: true, sort: (topic) => Number(topic.next_sequence) || 0, render: (topic) => <span className="font-mono text-ink-2">{String(topic.next_sequence ?? "")}</span> },
  ];

  return (
    <Page
      eyebrow="Train"
      title="Streams"
      sub={STREAMS_SUB}
      actions={
        permissions.canWrite ? (
          <Button variant="primary" onClick={() => setCreating(true)}>
            <Plus size={14} aria-hidden /> New topic
          </Button>
        ) : undefined
      }
    >
      {!creating && <PageError error={runner.error} />}
      {listing.isLoading ? (
        <Skeleton />
      ) : listing.isError ? (
        <ErrorBlock error={listing.error} retry={() => void listing.refetch()} what="streams" />
      ) : (
        <>
          <StatRow cols={3}>
            <Stat label="Topics" value={topics.length} icon={Radio} sub={listing.data?.next_cursor != null || cursor ? "on this page" : undefined} />
            <Stat label="Held messages" value={counted.length ? num(held) : "—"} icon={Inbox} />
            <Stat label="Subscribers" value={counted.length ? num(subscribers) : "—"} icon={Users} />
          </StatRow>
          <Toolbar>
            <form className="flex flex-wrap items-center gap-1.5" onSubmit={filter}>
              <input className="search-input" type="search" name="prefix" defaultValue={prefix} key={prefix} placeholder="Name prefix…" aria-label="Name prefix" />
              <label className="inline-flex items-center gap-1.5 text-[12.5px] text-ink-2">
                Page size
                <input type="number" name="limit" min={1} max={200} defaultValue={limit} key={limit} className="w-20" />
              </label>
              <Button type="submit">Filter</Button>
            </form>
          </Toolbar>
          {groups.length === 0 && (
            <Empty icon={Radio} title="No Topics">
              No topics match. Clear the prefix, or create a topic.
            </Empty>
          )}
          <div className="space-y-4">
            {groups.map((group) => (
              <Card key={group.zetta} label="Topics" title={group.zetta === "bus" ? "Lake Bus" : "Workspace"} padded={false} actions={<span className="font-mono text-[12px] text-ink-3">{(group.topics ?? []).length}</span>}>
                {group.error ? (
                  <div className="px-4 pt-3">
                    <Banner tone="warn">{group.error}</Banner>
                  </div>
                ) : null}
                <DataTable
                  rows={group.topics ?? []}
                  columns={columns(group.zetta)}
                  rowKey={(topic) => String(topic.topic ?? "")}
                  caption={group.zetta === "bus" ? "Lake bus topics" : "Workspace topics"}
                />
              </Card>
            ))}
          </div>
          <nav className="pagination mt-3 flex gap-2" aria-label="Streams pages">
            {listing.data?.previous_cursor != null && <Button to={pageQuery({ cursor: listing.data.previous_cursor })}>Previous</Button>}
            {listing.data?.next_cursor != null && <Button to={pageQuery({ cursor: listing.data.next_cursor })}>Next</Button>}
          </nav>
        </>
      )}
      {creating && permissions.canWrite && (
        <Drawer
          title="New Topic"
          subtitle="New topics live in this workspace."
          onClose={() => {
            setCreating(false);
            runner.setError(null);
          }}
          footer={
            <>
              <Button onClick={() => setCreating(false)}>Cancel</Button>
              <Button variant="primary" type="submit" form="new-topic" loading={runner.busy === "create"}>
                Create topic
              </Button>
            </>
          }
        >
          <FormError error={runner.error} />
          <form id="new-topic" className="stack" onSubmit={create}>
            <label>
              Topic name
              <input name="topic" required />
            </label>
            <label>
              Settings (JSON)
              <textarea name="settings" rows={10} required defaultValue={DEFAULT_SETTINGS} className="font-mono" />
            </label>
            <p className="muted m-0 text-[12.5px]">
              Configure retention_ms, max_events, memory_cap_bytes, disk_cap_bytes and delivery defaults. Null clears an optional override.
            </p>
          </form>
        </Drawer>
      )}
    </Page>
  );
}
