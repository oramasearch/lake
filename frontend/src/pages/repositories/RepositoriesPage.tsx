/** /repositories (web_ui_repositories): the repositories the viewer can read, newest first, and
 * "New repository" in a drawer. Figures on top (repositories, private and public, the workspaces
 * they live in), then search and filters by workspace and visibility. Visitors see the public
 * listing. Outside /w/ by design. `?customer_id=` narrows the list to one owner. */
import { useMemo, useState } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { FolderGit2, Globe, Layers, Lock, Plus } from "lucide-react";
import { useSession } from "@/lib/session";
import { Drawer } from "@/lib/overlay";
import { Banner, Button, DataTable, Empty, Field, fieldError, FilterChips, Form, Page, Query, SearchInput, Stat, StatRow, Toolbar } from "@/lib/ui";
import { date, relative } from "@/lib/format";
import { forgeAct, itemsOf, useForgeRead, Visibility, type Items, type Repository } from "./forge";

/** The workspace filter's key for a repository that has none yet. */
const NO_WORKSPACE = "-";

export default function RepositoriesPage() {
  const { viewer, workspaceId } = useSession();
  const [params] = useSearchParams();
  const customerId = params.get("customer_id") ?? "";
  const q = useForgeRead<Items<Repository>>("list_repositories", { limit: 500, customer_id: customerId });
  const spaces = viewer.signed_in ? viewer.spaces : [];
  const owners = new Map(spaces.map((space) => [space.id, space]));
  const workspaces = new Map(spaces.flatMap((space) => space.workspaces).map((workspace) => [workspace.id, workspace]));
  const repositories = itemsOf(q.data);
  const [search, setSearch] = useState("");
  const [workspace, setWorkspace] = useState<string | null>(null);
  const [visibility, setVisibility] = useState<string | null>(null);
  const [creating, setCreating] = useState(false);
  const canCreate = viewer.signed_in && spaces.length > 0;

  const ownerName = (r: Repository) => owners.get(r.customer_id)?.name ?? r.customer_id;
  const workspaceName = (id: string) => workspaces.get(id)?.name ?? id;

  const workspaceOptions = (() => {
    const counts = new Map<string, number>();
    for (const r of repositories) counts.set(r.workspace_id || NO_WORKSPACE, (counts.get(r.workspace_id || NO_WORKSPACE) ?? 0) + 1);
    return [...counts.entries()]
      .map(([id, count]) => ({ id, label: id === NO_WORKSPACE ? "No workspace" : workspaceName(id), count }))
      .sort((a, b) => String(a.label).localeCompare(String(b.label)));
  })();
  const visibilityOptions = useMemo(() => {
    const counts = new Map<string, number>();
    for (const r of repositories) counts.set(r.visibility, (counts.get(r.visibility) ?? 0) + 1);
    return [...counts.entries()].sort(([a], [b]) => a.localeCompare(b)).map(([id, count]) => ({ id, label: id, count }));
  }, [repositories]);

  const needle = search.trim().toLowerCase();
  const visible = repositories.filter(
    (r) =>
      (!needle || `${ownerName(r)}/${r.name} ${r.description ?? ""}`.toLowerCase().includes(needle)) &&
      (!workspace || (r.workspace_id || NO_WORKSPACE) === workspace) &&
      (!visibility || r.visibility === visibility),
  );
  const privateCount = repositories.filter((r) => r.visibility === "private").length;
  const publicCount = repositories.filter((r) => r.visibility === "public").length;
  const homes = new Set(repositories.map((r) => r.workspace_id).filter(Boolean));
  const unplaced = repositories.filter((r) => !r.workspace_id).length;
  const owner = customerId ? (owners.get(customerId)?.name ?? customerId) : "";

  const newRepository = canCreate ? (
    <Button variant="primary" onClick={() => setCreating(true)}>
      <Plus size={14} aria-hidden /> New repository
    </Button>
  ) : null;

  return (
    <Page
      eyebrow="Data"
      title={viewer.signed_in ? "Repositories" : "Public Repositories"}
      sub={viewer.signed_in ? "Code, files and history you can access, newest first." : "Browse code, files, and history shared publicly on this Lake."}
      actions={newRepository}
    >
      {customerId && (
        <Banner tone="quiet" action={<Button to="/repositories">Show all</Button>}>
          Showing the repositories of <strong>{owner}</strong>.
        </Banner>
      )}
      <Query
        q={q}
        what="repositories"
        lines={6}
        isEmpty={(value) => itemsOf(value).length === 0}
        empty={
          <Empty
            icon={FolderGit2}
            title={viewer.signed_in ? "No repositories yet" : "No public repositories yet"}
            action={viewer.signed_in ? newRepository : <Button to="/sign-up">Create an account</Button>}
          >
            {viewer.signed_in ? "Create your first repository to store files and review changes." : "Create an account to start your own."}
          </Empty>
        }
      >
        {() => (
          <>
            <StatRow cols={viewer.signed_in ? 4 : 3}>
              <Stat label="Repositories" value={repositories.length} icon={FolderGit2} sub={customerId ? `owned by ${owner}` : "you can read"} />
              <Stat label="Private" value={privateCount} icon={Lock} sub="members only" />
              <Stat label="Public" value={publicCount} icon={Globe} sub="anyone can browse" />
              {viewer.signed_in && <Stat label="Workspaces" value={homes.size} icon={Layers} sub={unplaced ? `${unplaced} need a workspace` : "they live in"} />}
            </StatRow>
            <Toolbar>
              <SearchInput value={search} onChange={setSearch} placeholder="Search repositories…" label="Search repositories" />
              {viewer.signed_in && workspaceOptions.length > 1 && <FilterChips options={workspaceOptions} value={workspace} onChange={setWorkspace} allLabel="All workspaces" />}
              {visibilityOptions.length > 1 && (
                <div className="ml-auto">
                  <FilterChips options={visibilityOptions} value={visibility} onChange={setVisibility} allLabel="Any visibility" />
                </div>
              )}
            </Toolbar>
            <DataTable<Repository>
              what="repositories"
              caption="Repositories"
              rows={visible}
              rowKey={(r) => r.id}
              empty={
                <Empty compact title="No repository matches">
                  Clear the search or a filter to see more.
                </Empty>
              }
              columns={[
                {
                  key: "repository",
                  label: "Repository",
                  sort: (r) => `${ownerName(r)}/${r.name}`,
                  render: (r) => (
                    <div className="flex min-w-0 items-start gap-2.5">
                      <FolderGit2 size={15} aria-hidden className="mt-0.5 shrink-0 text-ink-3" />
                      <div className="min-w-0">
                        <Link to={`/repository?id=${encodeURIComponent(r.id)}`} className="font-semibold">{`${ownerName(r)}/${r.name}`}</Link>
                        <div className="max-w-[34rem] truncate text-[12px] text-ink-3">
                          {r.description_unavailable !== undefined ? "Description unavailable" : r.description || "No description."}
                        </div>
                      </div>
                    </div>
                  ),
                },
                {
                  key: "owner",
                  label: "Owner",
                  sort: (r) => ownerName(r),
                  render: (r) => {
                    const space = owners.get(r.customer_id);
                    const name = space?.name ?? r.customer_id;
                    return <Link to={`/repositories?customer_id=${encodeURIComponent(r.customer_id)}`}>{space?.personal ? `${name} (you)` : name}</Link>;
                  },
                },
                {
                  key: "workspace",
                  label: "Workspace",
                  sort: (r) => (r.workspace_id ? workspaceName(r.workspace_id) : ""),
                  render: (r) => {
                    if (!r.workspace_id) return viewer.signed_in ? <span className="muted">Needs a workspace</span> : null;
                    const w = workspaces.get(r.workspace_id);
                    return w ? <Link to={`/w/${encodeURIComponent(w.id)}/design/projects`}>{w.name}</Link> : null;
                  },
                },
                { key: "visibility", label: "Visibility", sort: (r) => r.visibility, render: (r) => <Visibility value={r.visibility} /> },
                {
                  key: "updated",
                  label: "Last change",
                  sort: (r) => r.updated_at || r.created_at,
                  render: (r) => (
                    <span className="font-mono text-[12px] text-ink-2" title={`Created ${date(r.created_at)}`}>
                      {relative(r.updated_at || r.created_at)}
                    </span>
                  ),
                },
              ]}
            />
            <p className="muted mt-2 text-[12px]">
              {visible.length} of {repositories.length} repositories
            </p>
          </>
        )}
      </Query>
      {creating && <CreateRepository workspaceId={workspaceId} onClose={() => setCreating(false)} />}
    </Page>
  );
}

/** "New repository": one workspace choice per space; the chosen workspace names its space, so the
 * page sends that space as `customer_id` (serve_repository_create did it from the store). New
 * repositories are private. Success opens the repository. */
function CreateRepository({ workspaceId, onClose }: { workspaceId: string | null; onClose: () => void }) {
  const { viewer } = useSession();
  const navigate = useNavigate();
  const spaces = viewer.spaces.filter((space) => space.workspaces.length > 0);
  const all = spaces.flatMap((space) => space.workspaces);
  const [workspace, setWorkspace] = useState(all.find((w) => w.id === workspaceId)?.id ?? all[0]?.id ?? "");
  const [name, setName] = useState("");
  const [description, setDescription] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  if (!all.length) {
    return (
      <Drawer title="New Repository" onClose={onClose} footer={<Button onClick={onClose}>Close</Button>}>
        <Empty icon={Layers} title="Create a workspace first" action={<Button to="/workspaces">Create a workspace</Button>}>
          A repository belongs to one workspace.
        </Empty>
      </Drawer>
    );
  }
  const submit = async () => {
    const chosen = all.find((w) => w.id === workspace);
    if (!chosen) return;
    setBusy(true);
    setError(null);
    try {
      const created = await forgeAct<Repository>("create_repository", {
        customer_id: chosen.customer_id,
        workspace_id: chosen.id,
        name,
        description,
        visibility: "private",
      });
      navigate(`/repository?id=${encodeURIComponent(created.id)}`);
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  const formError = fieldError(error, "name") || fieldError(error, "description") || fieldError(error, "workspace_id") ? null : error;
  return (
    <Drawer
      title="New Repository"
      subtitle="A repository belongs to one workspace, and stays there."
      onClose={onClose}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button type="submit" form="create-repository" variant="primary" loading={busy}>
            Create repository
          </Button>
        </>
      }
    >
      <Form id="create-repository" onSubmit={submit} error={formError}>
        <Field label="Workspace" required error={fieldError(error, "workspace_id")}>
          <select value={workspace} onChange={(e) => setWorkspace(e.target.value)}>
            {spaces.map((space) => (
              <optgroup key={space.id} label={space.personal ? "Your workspaces" : space.name}>
                {space.workspaces.map((w) => (
                  <option key={w.id} value={w.id}>
                    {w.name}
                  </option>
                ))}
              </optgroup>
            ))}
          </select>
        </Field>
        <Field label="Repository name" required help="Use lowercase letters, numbers, and inner hyphens." error={fieldError(error, "name")}>
          <input value={name} onChange={(e) => setName(e.target.value)} pattern="[a-z0-9]([a-z0-9\-]{0,254}[a-z0-9])?" placeholder="my-first-repository" autoFocus />
        </Field>
        <Field label="Description" error={fieldError(error, "description")}>
          <input value={description} onChange={(e) => setDescription(e.target.value)} />
        </Field>
        <Banner tone="quiet">
          <Lock size={12} aria-hidden className="mr-1 inline" />
          New repositories are private.
        </Banner>
      </Form>
    </Drawer>
  );
}
