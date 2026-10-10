/** The audit views: `/audit` (the operator's Audit Log, every log) and `/activity` (Your Activity,
 * the signed-in person's own events). One renderer on the server (web_ui_audit) behind
 * serve_audit_page and serve_activity_page. */
import { useLocation, useSearchParams } from "react-router-dom";
import { useRead, type Row } from "@/lib/api";
import { useSession } from "@/lib/session";
import { DataTable, Page, Status, type Column } from "@/lib/ui";
import { PanelEmpty, When, useMetaTrail, useQuerySubmit } from "@/pages/organizations/shared";

export default function AuditPage() {
  const { pathname } = useLocation();
  const own = pathname === "/activity";
  const [params] = useSearchParams();
  const { workspaces } = useSession();
  const text = (name: string) => params.get(name) ?? "";
  const newest = text("order") !== "oldest";
  const includeReads = own ? text("reads") === "all" : true;
  const input = {
    limit: 500,
    order: newest ? "newest" : "oldest",
    principal: own ? undefined : text("principal"),
    object: text("object"),
    outcome: text("outcome"),
    from: text("from"),
    to: text("to"),
    // /audit passes no include_reads (the capability's default); /activity says which.
    include_reads: own ? includeReads : undefined,
  };
  const events = useRead<Row>(own ? "my_audit" : "audit", input);
  // The checkpoint panel shows only when this lake has a checkpoint service.
  const log = useRead<Row>(own ? null : "transparency_log_configuration", undefined, { retry: false });
  const submit = useQuerySubmit(own ? "/activity" : "/audit");
  const items: Row[] | undefined = events.data?.items;
  useMetaTrail(items ? `${items.length} events` : null, own ? "Activity" : "Audit");

  const workspaceLabel = (id: string) => workspaces.find((w) => w.id === id)?.name ?? id.slice(0, 8);
  const columns: Column<Row>[] = [
    { key: "when", label: "When", render: (e) => <When value={e.occurred_at} /> },
    {
      key: "principal",
      label: "Principal",
      // The principal's name, with the recorded identity kept as its title.
      render: (e) => (e.principal_name && e.principal_name !== e.principal ? <span title={e.principal}>{e.principal_name}</span> : <code>{e.principal}</code>),
    },
    { key: "object", label: "Object", render: (e) => <code>{e.object}</code> },
    {
      key: "action",
      label: "Action",
      render: (e) =>
        e.capability ? (
          <>
            {e.action} <code>{e.capability}</code>
          </>
        ) : (
          e.action
        ),
    },
    { key: "outcome", label: "Outcome", render: (e) => <Status value={e.outcome} /> },
    {
      key: "log",
      label: "Log",
      render: (e) => (e.log === "workspace" ? <span title={e.workspace_id}>Workspace {workspaceLabel(e.workspace_id ?? "")}</span> : "Central"),
    },
    { key: "digest", label: "Digest", render: (e) => (e.pending ? <em>being written</em> : <code>{e.digest}</code>) },
  ];

  return (
    <Page
      eyebrow={own ? "Organization" : "control plane"}
      title={own ? "Your Activity" : "Audit Log"}
      sub={own ? "Signed, hash-linked events for everything you did on this lake." : "Signed, hash-linked events. Use the filters to narrow the evidence view."}
    >
      <form className="filters" onSubmit={submit} key={params.toString()}>
        {!own && (
          <label>
            Principal
            <input name="principal" defaultValue={text("principal")} />
          </label>
        )}
        <label>
          Object
          <input name="object" defaultValue={text("object")} />
        </label>
        <label>
          Outcome
          <input name="outcome" defaultValue={text("outcome")} />
        </label>
        <label>
          From
          <input name="from" defaultValue={text("from")} />
        </label>
        <label>
          To
          <input name="to" defaultValue={text("to")} />
        </label>
        <label>
          Order
          <select name="order" defaultValue={newest ? "newest" : "oldest"}>
            <option value="newest">Newest first</option>
            <option value="oldest">Oldest first</option>
          </select>
        </label>
        {own && (
          <label>
            <input type="checkbox" name="reads" value="all" defaultChecked={includeReads} /> Include reads
          </label>
        )}
        <button type="submit">Filter</button>
      </form>
      {own && !includeReads && <p className="sub">Successful reads, such as the polling this page and the workspace views do, are hidden. Failed and denied reads are shown.</p>}
      {log.data && (
        <div className="panel">
          <h2>Transparency Log</h2>
          <p className="sub">
            Effective mode: <code>{log.data.mode}</code>; witness quorum: {log.data.witness_threshold ?? 0} of {(log.data.witnesses ?? []).length}
            {log.data.aggregator_endpoint !== undefined ? "; aggregation endpoint configured" : ""}.
          </p>
          <p className="sub">
            API: <code>GET /v1/transparency_log_configuration</code>.
          </p>
        </div>
      )}
      {/* Decision 52: the central log and each workspace's own audit; say which could not be read. */}
      {((events.data?.unread_workspaces ?? []) as Row[]).map((unread) => (
        <div key={unread.workspace_id} className="panel" role="status">
          The audit of workspace <code>{unread.workspace_id}</code> could not be read: {unread.message}
        </div>
      ))}
      <DataTable q={events} what="audit events" rows={items} columns={columns} rowKey={(e) => e.id} empty={<PanelEmpty>No audit events match these filters.</PanelEmpty>} />
      <p className="sub">
        API: <code>GET /v1/audit</code>, <code>GET /v1/export_audit</code>, and <code>GET /v1/verify_audit</code>.
      </p>
    </Page>
  );
}
