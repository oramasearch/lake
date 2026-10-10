/** The audit views: `/audit` (the operator's Audit Log, every log) and `/activity` (Your Activity,
 * the signed-in person's own events). One renderer on the server (web_ui_audit) behind
 * serve_audit_page and serve_activity_page. The outcome, the order and "Include reads" are chips
 * over the list; principal, object and the time range sit under "Filters"; all of them are the
 * URL's query, read by the capability. The search box narrows the rows already read. */
import { useState } from "react";
import { useLocation, useNavigate, useSearchParams } from "react-router-dom";
import { ArrowDownUp, CheckCircle2, ListFilter, ScrollText, ShieldAlert, XCircle } from "lucide-react";
import { useRead, type Row } from "@/lib/api";
import { useSession } from "@/lib/session";
import { Banner, Button, Card, Chip, DataTable, FilterChips, KeyValue, Mono, Page, SearchInput, Stat, StatRow, Status, Toolbar, cx, toneOf, type Column } from "@/lib/ui";
import { NameCell, ObjectChip, PanelEmpty, When, matches, useMetaTrail, useQuerySubmit } from "@/pages/organizations/shared";

const OUTCOMES = ["success", "denied", "failure"] as const;

export default function AuditPage() {
  const { pathname } = useLocation();
  const own = pathname === "/activity";
  const path = own ? "/activity" : "/audit";
  const [params] = useSearchParams();
  const navigate = useNavigate();
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
  const submit = useQuerySubmit(path);
  const items: Row[] | undefined = events.data?.items;
  useMetaTrail(items ? `${items.length} events` : null, own ? "Activity" : "Audit");
  const narrowed = !!(text("principal") || text("object") || text("from") || text("to"));
  const [filtersOpen, setFiltersOpen] = useState(narrowed);
  const [search, setSearch] = useState("");

  /** The same address with one query parameter changed ("" removes it). */
  const setParam = (name: string, value: string) => {
    const next = new URLSearchParams(params);
    if (value) next.set(name, value);
    else next.delete(name);
    const q = next.toString();
    navigate(q ? `${path}?${q}` : path);
  };

  const workspaceLabel = (id: string) => workspaces.find((w) => w.id === id)?.name ?? id.slice(0, 8);
  const rows = items?.filter((e) => matches(search, e.action, e.capability, e.object, e.principal, e.principal_name));
  const count = (outcome: string) => (items ?? []).filter((e) => e.outcome === outcome).length;
  const columns: Column<Row>[] = [
    {
      key: "when",
      label: "When",
      render: (e) => (
        <span className="inline-flex items-center gap-2">
          <span
            aria-hidden
            className={cx("inline-block h-2 w-2 shrink-0 rounded-full", toneOf(e.outcome) === "success" ? "bg-ok" : toneOf(e.outcome) === "danger" || e.outcome === "failure" ? "bg-bad" : "bg-warn")}
          />
          <When value={e.occurred_at} />
        </span>
      ),
    },
    { key: "action", label: "Action", render: (e) => <NameCell name={<span className="font-medium">{e.action}</span>} sub={e.capability ? <code>{e.capability}</code> : undefined} /> },
    {
      key: "principal",
      label: "Principal",
      // The principal's name, with the recorded identity kept as its title.
      render: (e) => (e.principal_name && e.principal_name !== e.principal ? <span title={e.principal}>{e.principal_name}</span> : <Mono short={28}>{e.principal}</Mono>),
    },
    { key: "object", label: "Object", render: (e) => <ObjectChip value={e.object} /> },
    { key: "outcome", label: "Outcome", render: (e) => <Status value={e.outcome} tone={e.outcome === "failure" ? "danger" : undefined} /> },
    {
      key: "log",
      label: "Log",
      render: (e) =>
        e.log === "workspace" ? (
          <Chip tone="action" title={e.workspace_id}>
            Workspace {workspaceLabel(e.workspace_id ?? "")}
          </Chip>
        ) : (
          <Chip>Central</Chip>
        ),
    },
    { key: "digest", label: "Digest", render: (e) => (e.pending ? <em className="text-ink-3">being written</em> : <Mono short={12}>{e.digest}</Mono>) },
  ];

  return (
    <Page
      eyebrow={own ? "Organization" : "Administration"}
      title={own ? "Your Activity" : "Audit Log"}
      sub={own ? "Signed, hash-linked events for everything you did on this lake." : "Signed, hash-linked events. Use the filters to narrow the evidence view."}
    >
      {/* Decision 52: the central log and each workspace's own audit; say which could not be read. */}
      {((events.data?.unread_workspaces ?? []) as Row[]).map((unread) => (
        <Banner key={unread.workspace_id} tone="warn">
          The audit of workspace <code>{unread.workspace_id}</code> could not be read: {unread.message}
        </Banner>
      ))}
      {items && (
        <StatRow>
          <Stat label="Events" value={items.length === 500 ? "500+" : items.length} icon={ScrollText} sub={newest ? "newest first" : "oldest first"} />
          <Stat label="Succeeded" value={count("success")} icon={CheckCircle2} />
          <Stat label="Denied" value={count("denied")} icon={ShieldAlert} tone={count("denied") ? "bad" : undefined} />
          <Stat label="Failed" value={count("failure")} icon={XCircle} tone={count("failure") ? "bad" : undefined} />
        </StatRow>
      )}
      <Toolbar>
        <SearchInput value={search} onChange={setSearch} placeholder="Search these events…" />
        <FilterChips
          options={OUTCOMES.map((o) => ({ id: o, label: o === "failure" ? "Failed" : o === "denied" ? "Denied" : "Succeeded" }))}
          value={(OUTCOMES as readonly string[]).includes(text("outcome")) ? (text("outcome") as (typeof OUTCOMES)[number]) : null}
          onChange={(v) => setParam("outcome", v ?? "")}
          allLabel="Any outcome"
        />
        {own && (
          <Chip onClick={() => setParam("reads", includeReads ? "" : "all")} active={includeReads}>
            Include reads
          </Chip>
        )}
        <div className="ml-auto flex flex-wrap items-center gap-2">
          <Button onClick={() => setParam("order", newest ? "oldest" : "")} title="Change the order">
            <ArrowDownUp size={14} aria-hidden /> {newest ? "Newest first" : "Oldest first"}
          </Button>
          <Button onClick={() => setFiltersOpen((v) => !v)} variant={narrowed ? "primary" : "secondary"}>
            <ListFilter size={14} aria-hidden /> Filters
          </Button>
        </div>
      </Toolbar>
      {filtersOpen && (
        <form className="filters" onSubmit={submit} key={params.toString()}>
          {!own && (
            <label>
              Principal
              <input name="principal" defaultValue={text("principal")} />
            </label>
          )}
          <label>
            Object
            <input name="object" defaultValue={text("object")} placeholder="repository/…" />
          </label>
          {/* A free outcome the chips do not name stays editable here. */}
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
      )}
      {own && !includeReads && <Banner tone="quiet">Successful reads, such as the polling this page and the workspace views do, are hidden. Failed and denied reads are shown.</Banner>}
      <DataTable
        q={events}
        what="audit events"
        rows={rows}
        columns={columns}
        rowKey={(e) => e.id}
        empty={<PanelEmpty title="No audit events">{items?.length ? "No event matches the search." : "No audit events match these filters."}</PanelEmpty>}
      />
      {log.data && (
        <Card label="Transparency log" className="mt-4">
          <KeyValue
            rows={[
              ["Effective mode", <code>{log.data.mode}</code>],
              ["Witness quorum", `${log.data.witness_threshold ?? 0} of ${(log.data.witnesses ?? []).length}`],
              ["Aggregation", log.data.aggregator_endpoint !== undefined ? "endpoint configured" : "—"],
              ["API", <code>GET /v1/transparency_log_configuration</code>],
            ]}
          />
        </Card>
      )}
      <p className="muted mt-3 text-[12.5px]">
        API: <code>GET /v1/audit</code>, <code>GET /v1/export_audit</code>, and <code>GET /v1/verify_audit</code>.
      </p>
    </Page>
  );
}
