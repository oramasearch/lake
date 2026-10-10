/** /repositories (web_ui_repositories): the repositories the viewer can read, newest first, and
 * "Create a Repository". Visitors see the public listing. Outside /w/ by design. */
import { useState } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { useSession } from "@/lib/session";
import { Badge, Button, DataTable, Field, fieldError, Form } from "@/lib/ui";
import { date } from "@/lib/format";
import { forgeAct, itemsOf, useForgeRead, type Items, type Repository } from "./forge";

export default function RepositoriesPage() {
  const { viewer, workspaceId } = useSession();
  const [params] = useSearchParams();
  const customerId = params.get("customer_id") ?? "";
  const q = useForgeRead<Items<Repository>>("list_repositories", { limit: 500, customer_id: customerId });
  const spaces = viewer.signed_in ? viewer.spaces : [];
  const owners = new Map(spaces.map((space) => [space.id, space]));
  const workspaces = new Map(spaces.flatMap((space) => space.workspaces).map((workspace) => [workspace.id, workspace]));
  const repositories = itemsOf(q.data);

  return (
    <div className="page-container">
      <header className="page-header">
        <div className="min-w-0">
          <p className="eyebrow">{viewer.signed_in ? "Your work" : "Explore"}</p>
          <h1>{viewer.signed_in ? "Repositories" : "Public Repositories"}</h1>
          <p className="sub">{viewer.signed_in ? "Repositories you can access, newest first." : "Browse code, files, and history shared publicly on this Lake."}</p>
        </div>
      </header>
      <DataTable<Repository>
        q={q}
        what="repositories"
        caption="Repositories"
        rows={repositories}
        rowKey={(r) => r.id}
        empty={
          <div className="panel empty">
            {viewer.signed_in ? (
              "No repositories yet. Create your first repository to store files and review changes."
            ) : (
              <>
                No public repositories yet. <Link to="/sign-up">Create an account</Link> to start your own.
              </>
            )}
          </div>
        }
        columns={[
          {
            key: "repository",
            label: "Repository",
            render: (r) => <Link to={`/repository?id=${encodeURIComponent(r.id)}`}>{`${owners.get(r.customer_id)?.name ?? r.customer_id}/${r.name}`}</Link>,
          },
          {
            key: "owner",
            label: "Owner",
            render: (r) => {
              const owner = owners.get(r.customer_id);
              const name = owner?.name ?? r.customer_id;
              return <Link to={`/repositories?customer_id=${encodeURIComponent(r.customer_id)}`}>{owner?.personal ? `${name} (you)` : name}</Link>;
            },
          },
          {
            key: "workspace",
            label: "Workspace",
            render: (r) => {
              if (!r.workspace_id) return viewer.signed_in ? <span className="muted">Needs a workspace</span> : null;
              const workspace = workspaces.get(r.workspace_id);
              return workspace ? <Link to={`/w/${encodeURIComponent(workspace.id)}/design/projects`}>{workspace.name}</Link> : null;
            },
          },
          { key: "visibility", label: "Visibility", render: (r) => <Badge>{r.visibility}</Badge> },
          {
            key: "description",
            label: "Description",
            render: (r) => (r.description_unavailable !== undefined ? <span className="muted">Unavailable</span> : r.description),
          },
          { key: "created", label: "Created", render: (r) => date(r.created_at), sort: (r) => r.created_at },
        ]}
      />
      {viewer.signed_in && spaces.length > 0 && <CreateRepository workspaceId={workspaceId} />}
    </div>
  );
}

/** "Create a Repository": one workspace choice per space; the chosen workspace names its space,
 * so the page sends that space as `customer_id` (serve_repository_create did it from the store).
 * New repositories are private. Success opens the repository. */
function CreateRepository({ workspaceId }: { workspaceId: string | null }) {
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
      <section className="panel">
        <h2>Create a Repository</h2>
        <p>
          A repository belongs to one workspace of a space. <Link to="/workspaces">Create a workspace</Link> first.
        </p>
      </section>
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
    <section className="panel">
      <h2>Create a Repository</h2>
      <p>
        A repository belongs to one workspace, and stays there. Your own workspaces are marked <em>yours</em> below.
      </p>
      <Form onSubmit={submit} error={formError}>
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
          <input value={name} onChange={(e) => setName(e.target.value)} pattern="[a-z0-9]([a-z0-9\-]{0,254}[a-z0-9])?" placeholder="my-first-repository" />
        </Field>
        <Field label="Description" error={fieldError(error, "description")}>
          <input value={description} onChange={(e) => setDescription(e.target.value)} />
        </Field>
        <p>New repositories are private.</p>
        <div>
          <Button type="submit" variant="primary" loading={busy}>
            Create repository
          </Button>
        </div>
      </Form>
    </section>
  );
}

