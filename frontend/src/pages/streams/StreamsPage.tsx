/** Train → Streams (web_ui_streams.cpp, list mode; serve_streams_page): the workspace Zetta's and
 * the Lake bus's topics, paged, and a New Topic form for those who may write. */
import { type FormEvent } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { act, useRead, type Row } from "@/lib/api";
import { useSession } from "@/lib/session";
import { Badge, ErrorBlock, Page, Skeleton } from "@/lib/ui";
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

  return (
    <Page eyebrow="Train" title="Streams" sub={STREAMS_SUB}>
      <PageError error={runner.error} />
      <form className="filters" onSubmit={filter}>
        <label>
          Name prefix
          <input name="prefix" defaultValue={prefix} key={prefix} />
        </label>
        <label>
          Page size
          <input type="number" name="limit" min={1} max={200} defaultValue={limit} key={limit} />
        </label>
        <button type="submit">Filter</button>
      </form>
      {listing.isLoading ? (
        <Skeleton />
      ) : listing.isError ? (
        <ErrorBlock error={listing.error} retry={() => void listing.refetch()} what="streams" />
      ) : (
        <>
          {(listing.data?.groups ?? []).map((group) => (
            <section key={group.zetta} className="panel">
              <h2>{group.zetta === "bus" ? "Lake Bus" : "Workspace Zetta"}</h2>
              {group.error ? <p className="error">{group.error}</p> : null}
              <div className="table-wrap">
                <table>
                  <thead>
                    <tr>
                      <th>Topic</th>
                      <th>Subscribers</th>
                      <th>Held Events</th>
                      <th>Next Sequence</th>
                    </tr>
                  </thead>
                  <tbody>
                    {(group.topics ?? []).map((topic) => {
                      const name = String(topic.topic ?? "");
                      const counts = topic.counts_available !== false;
                      return (
                        <tr key={name}>
                          <td>
                            <Link to={ws("/train/stream") + qs({ customer_id: customerId, zetta: group.zetta, topic: name })}>{String(topic.display_name ?? displayName(name))}</Link>
                            {topic.system ? (
                              <>
                                {" "}
                                <Badge>system</Badge>
                              </>
                            ) : null}
                          </td>
                          <td>{counts ? String(topic.subscribers ?? "") : "—"}</td>
                          <td>{counts ? String(topic.held_events ?? "") : "—"}</td>
                          <td>{String(topic.next_sequence ?? "")}</td>
                        </tr>
                      );
                    })}
                  </tbody>
                </table>
              </div>
            </section>
          ))}
          <nav className="pagination" aria-label="Streams pages">
            {listing.data?.previous_cursor != null && <Link to={pageQuery({ cursor: listing.data.previous_cursor })}>Previous</Link>}
            {listing.data?.next_cursor != null && <Link to={pageQuery({ cursor: listing.data.next_cursor })}>Next</Link>}
          </nav>
        </>
      )}
      {permissions.canWrite && (
        <section className="panel">
          <h2>New Topic</h2>
          <form className="stack" onSubmit={create}>
            <label>
              Topic name
              <input name="topic" required />
            </label>
            <label>
              Settings (JSON)
              <textarea name="settings" rows={10} required defaultValue={DEFAULT_SETTINGS} />
            </label>
            <p>
              Configure retention_ms, max_events, memory_cap_bytes, disk_cap_bytes and delivery defaults. Null clears an optional override. New topics live on this space's workspace Zetta.
            </p>
            <button type="submit" disabled={runner.busy === "create"}>
              Create topic
            </button>
          </form>
        </section>
      )}
    </Page>
  );
}
