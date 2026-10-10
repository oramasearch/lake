/** The space and workspace picker in the top bar. Choosing a workspace keeps the page: a workspace
 * page reopens under the new workspace, any other page goes to its Projects. The choice lives in the
 * URL (`/w/<id>/…`); this browser remembers the last one for unprefixed links. */
import { useLocation, useNavigate } from "react-router-dom";
import { Link } from "react-router-dom";
import { useSession } from "@/lib/session";
import { isWorkspacePath, localPath } from "@/routes";

export function WorkspacePicker() {
  const { viewer, space, workspaceId } = useSession();
  const location = useLocation();
  const navigate = useNavigate();
  const spaceId = space?.id ?? viewer.customer_id ?? viewer.spaces[0]?.id ?? "";
  const current = viewer.spaces.find((s) => s.id === spaceId) ?? viewer.spaces[0];

  const open = (id: string) => {
    const path = localPath(location.pathname);
    navigate(isWorkspacePath(path) ? `/w/${encodeURIComponent(id)}${path}${location.search}` : `/w/${encodeURIComponent(id)}/design/projects`);
  };

  return (
    <form className="filters workspace-picker" onSubmit={(e) => e.preventDefault()}>
      <label>
        Space
        <select
          value={current?.id ?? ""}
          onChange={(e) => {
            const next = viewer.spaces.find((s) => s.id === e.target.value);
            const live = next?.workspaces ?? [];
            if (live.length === 1) open(live[0].id);
            else navigate(`/workspaces?customer_id=${encodeURIComponent(e.target.value)}`);
          }}
        >
          {viewer.spaces.map((s) => (
            <option key={s.id} value={s.id}>
              {s.label}
            </option>
          ))}
        </select>
      </label>
      <label>
        Workspace
        <select value={workspaceId && current?.workspaces.some((w) => w.id === workspaceId) ? workspaceId : ""} onChange={(e) => e.target.value && open(e.target.value)}>
          {!(workspaceId && current?.workspaces.some((w) => w.id === workspaceId)) && <option value="">Choose a workspace</option>}
          {(current?.workspaces ?? []).map((w) => (
            <option key={w.id} value={w.id}>
              {w.name}
            </option>
          ))}
        </select>
      </label>
      <Link className="workspace-new" to={`/workspaces?customer_id=${encodeURIComponent(current?.id ?? "")}`}>
        New workspace
      </Link>
    </form>
  );
}
