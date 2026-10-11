/** Design → Projects (lake's workspace_content, projects branch): the workspace's projects with their
 * figures, a way to the Opportunities, and New Project in a drawer. Each row's intent, pipelines
 * and status come from the project's own `get_project`, read alongside the list (the same read the
 * project page makes, so opening one is instant). */
import { useMemo, useState } from "react";
import { Link, useNavigate } from "react-router-dom";
import { useQueries } from "@tanstack/react-query";
import { FolderKanban, Lightbulb, Plus, SearchX } from "lucide-react";
import { Drawer } from "@/lib/overlay";
import { Banner, Button, DataTable, Empty, Field, FilterChips, Form, Page, Query, SearchInput, Stat, StatRow, Status, Toolbar, fieldError } from "@/lib/ui";
import { WorkspaceGate, useWorkspaceStatus } from "@/components/WorkspaceGate";
import { qs, useScope, useWq, useWsPath, wa, wq } from "./api";
import { PROJECT_STATE_TONE, projectState, runIsActive, text, withoutProjectAgentChecks, type J, type ProjectState } from "./logic";
import { FormError, useDesignTrail } from "./parts";
import "@/styles/design.css";

export default function ProjectsPage() {
  useDesignTrail({ label: "Projects", path: "/design/projects" }, [], "Projects");
  const ready = useWorkspaceStatus().q.data?.ready === true;
  const [creating, setCreating] = useState(false);
  return (
    <Page
      eyebrow="Design"
      title="Projects"
      sub="A project starts from the business question: the decision it should improve and how you will measure it. Its pipelines are built from that intent."
      actions={
        ready ? (
          <Button variant="primary" onClick={() => setCreating(true)}>
            <Plus size={14} aria-hidden /> New project
          </Button>
        ) : undefined
      }
    >
      <WorkspaceGate>
        <Projects onNew={() => setCreating(true)} />
        {creating && <NewProject onClose={() => setCreating(false)} />}
      </WorkspaceGate>
    </Page>
  );
}


type ProjectRow = { name: string; line: string; pipelines: number | null; approved: number | null; state: ProjectState | null };

function Projects({ onNew }: { onNew: () => void }) {
  const scope = useScope();
  const ws = useWsPath();
  const [search, setSearch] = useState("");
  const [state, setState] = useState<ProjectState | null>(null);
  const listed = useWq<{ projects?: unknown[] }>("list_projects");
  const runs = useWq<J>("list_runs");
  const names = useMemo(
    () => (Array.isArray(listed.data?.projects) ? listed.data!.projects! : []).map((entry) => (typeof entry === "string" ? entry : JSON.stringify(entry))),
    [listed.data],
  );
  const details = useQueries({
    queries: names.map((name) => ({
      queryKey: ["workspace_query", scope.customerId, scope.workspaceId, "get_project", { name }],
      queryFn: ({ signal }: { signal: AbortSignal }) => wq<J>(scope, "get_project", { name }, signal),
      staleTime: 30_000,
    })),
  });

  const rows: ProjectRow[] = names.map((name, index) => {
    const detail = details[index]?.data as J | undefined;
    if (!detail) return { name, line: "", pipelines: null, approved: null, state: null };
    const { state, pipelines, approved } = projectState(withoutProjectAgentChecks<J>(detail.graphs));
    return { name, line: text(detail, "intent") || text(detail, "description"), pipelines, approved, state };
  });
  const known = rows.every((r) => r.pipelines !== null);
  const counts = new Map<ProjectState, number>();
  for (const r of rows) if (r.state) counts.set(r.state, (counts.get(r.state) ?? 0) + 1);
  const needle = search.trim().toLowerCase();
  const shown = rows.filter((r) => (!state || r.state === state) && (!needle || `${r.name} ${r.line}`.toLowerCase().includes(needle)));
  const allRuns: J[] = !runs.isError && Array.isArray(runs.data?.runs) ? runs.data!.runs : [];

  const opportunities = (
    <Banner
      action={
        <Button to={ws("/design/opportunities")}>
          <Lightbulb size={14} aria-hidden /> See your Opportunities
        </Button>
      }
    >
      Not sure where to start? Auto-Suggest recommends what your data could be used for.
    </Banner>
  );

  return (
    <Query
      q={listed}
      what="projects"
      isEmpty={() => names.length === 0}
      empty={
        <>
          <Empty
            icon={FolderKanban}
            title="No Projects Yet"
            action={
              <>
                <Button variant="primary" onClick={onNew}>
                  <Plus size={14} aria-hidden /> New project
                </Button>
                <Button to={ws("/design/opportunities")}>See your Opportunities</Button>
              </>
            }
          >
            A project starts from the decision it should improve. Or let Auto-Suggest recommend one from your data.
          </Empty>
        </>
      }
    >
      {() => (
        <>
          <StatRow cols={runs.data && !runs.isError ? 3 : 2}>
            <Stat label="Projects" value={names.length} icon={FolderKanban} sub="in this workspace" />
            <Stat
              label="With pipelines"
              value={known ? rows.filter((r) => (r.pipelines ?? 0) > 0).length : "—"}
              sub={known ? `${rows.filter((r) => (r.approved ?? 0) > 0).length} with an approved one` : "reading the projects…"}
            />
            {runs.data && !runs.isError && (
              <Stat label="Runs active" value={allRuns.filter(runIsActive).length} sub={`of ${allRuns.length} runs listed`} to={ws("/train/runs")} />
            )}
          </StatRow>
          {opportunities}
          <Toolbar>
            <SearchInput value={search} onChange={setSearch} placeholder="Search projects…" label="Search projects" />
            <FilterChips
              options={(["approved", "proposed", "no pipelines"] as ProjectState[]).filter((s) => counts.has(s)).map((s) => ({ id: s, label: s.charAt(0).toUpperCase() + s.slice(1), count: counts.get(s) }))}
              value={state}
              onChange={setState}
            />
          </Toolbar>
          <DataTable<ProjectRow>
            rows={shown}
            q={listed}
            what="projects"
            caption="Projects"
            rowKey={(r) => r.name}
            initialSort={{ key: "name", dir: "ascending" }}
            empty={
              <Empty compact icon={SearchX} title="No Project Matches">
                Clear the search or the filter to see more.
              </Empty>
            }
            columns={[
              {
                key: "name",
                label: "Project",
                sort: (r) => r.name.toLowerCase(),
                render: (r) => (
                  <div className="min-w-0">
                    <Link to={ws(`/design/project?${qs({ name: r.name })}`)} className="font-semibold">
                      {r.name}
                    </Link>
                    <div className="max-w-[60ch] truncate text-[12px] text-ink-3" title={r.line}>
                      {r.line || (r.pipelines === null ? "…" : "No intent recorded")}
                    </div>
                  </div>
                ),
              },
              { key: "pipelines", label: "Pipelines", numeric: true, sort: (r) => r.pipelines, render: (r) => <span className="font-mono">{r.pipelines ?? "—"}</span> },
              { key: "approved", label: "Approved", numeric: true, sort: (r) => r.approved, render: (r) => <span className="font-mono">{r.approved ?? "—"}</span> },
              { key: "state", label: "Status", sort: (r) => r.state, render: (r) => (r.state ? <Status value={r.state} tone={PROJECT_STATE_TONE[r.state]} /> : <span className="text-ink-3">—</span>) },
            ]}
          />
        </>
      )}
    </Query>
  );
}

function NewProject({ onClose }: { onClose: () => void }) {
  const scope = useScope();
  const ws = useWsPath();
  const navigate = useNavigate();
  const [name, setName] = useState("");
  const [description, setDescription] = useState("");
  const [intent, setIntent] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const submit = async () => {
    setBusy(true);
    setError(null);
    try {
      // An empty description or intent is left out, as the form handler did.
      await wa(scope, "create_project", { name, ...(description ? { description } : {}), ...(intent ? { intent } : {}) });
      navigate(ws(`/design/project?${qs({ name })}`));
    } catch (e) {
      setError(e);
      setBusy(false);
    }
  };
  const inField = ["name", "description", "intent"].some((f) => fieldError(error, f) || fieldError(error, `input/${f}`));
  return (
    <Drawer
      title="New Project"
      subtitle="Start from the decision it should improve; lake builds its pipelines from that."
      onClose={onClose}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button type="submit" form="new-project" variant="primary" loading={busy}>
            Create project
          </Button>
        </>
      }
    >
      <Form id="new-project" onSubmit={submit}>
        <FormError error={inField ? null : error} />
        <Field label="Name" required help="Lowercase letters, digits, - and _." error={fieldError(error, "name") ?? fieldError(error, "input/name")}>
          <input name="name" pattern="[a-z0-9][a-z0-9_\-]{0,62}" placeholder="retention-review" value={name} onChange={(e) => setName(e.target.value)} />
        </Field>
        <Field label="Description" error={fieldError(error, "description") ?? fieldError(error, "input/description")}>
          <input name="description" placeholder="Which enterprise accounts will not renew?" value={description} onChange={(e) => setDescription(e.target.value)} />
        </Field>
        <Field label="The decision it should improve" error={fieldError(error, "intent") ?? fieldError(error, "input/intent")}>
          <textarea
            name="intent"
            rows={4}
            placeholder="Which accounts the team calls first each week, measured by accounts saved per quarter"
            value={intent}
            onChange={(e) => setIntent(e.target.value)}
          />
        </Field>
      </Form>
    </Drawer>
  );
}
