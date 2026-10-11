/** The workspace picker in the top bar (lake#638): one pill naming the current workspace, its owner
 * beside it only when the viewer has more than one, which opens the viewer's workspaces grouped by
 * owner (their account, then each organization). Past eight workspaces the menu opens with a filter
 * box; the list scrolls; one "New workspace" at its foot opens Workspaces, whose dialog asks which
 * owner. Choosing a workspace keeps the page: a workspace page reopens under the new workspace, any
 * other page goes to its Projects. The choice lives in the URL (`/w/<id>/…`); this browser
 * remembers the last one for unprefixed links. Pure rules: lib/workspacePicker.ts. */
import { useEffect, useRef, useState } from "react";
import { Link, useLocation, useNavigate } from "react-router-dom";
import { Check, ChevronDown, Layers, Plus, Search } from "lucide-react";
import { useSession, type SpaceChoice } from "@/lib/session";
import { isWorkspacePath, localPath } from "@/routes";
import { cx } from "@/lib/ui";
import { filterGroups, firstMatch, initialOf, needsFilter, newWorkspacePath, orderOwners, ownerKind, ownerName, pillOwner, pillTitle, showsOwner } from "@/lib/workspacePicker";

export function WorkspacePicker() {
  const { viewer, space, workspace, workspaceId } = useSession();
  const location = useLocation();
  const navigate = useNavigate();
  const [open, setOpen] = useState(false);
  const [filter, setFilter] = useState("");
  const box = useRef<HTMLDivElement>(null);
  useEffect(() => {
    setOpen(false);
  }, [location.pathname]);
  useEffect(() => {
    if (!open) {
      setFilter("");
      return;
    }
    const close = (e: MouseEvent) => {
      if (!box.current?.contains(e.target as Node)) setOpen(false);
    };
    const key = (e: KeyboardEvent) => {
      if (e.key === "Escape") setOpen(false);
    };
    document.addEventListener("mousedown", close);
    document.addEventListener("keydown", key);
    return () => {
      document.removeEventListener("mousedown", close);
      document.removeEventListener("keydown", key);
    };
  }, [open]);

  const go = (id: string) => {
    const path = localPath(location.pathname);
    navigate(isWorkspacePath(path) ? `/w/${encodeURIComponent(id)}${path}${location.search}` : `/w/${encodeURIComponent(id)}/design/projects`);
    setOpen(false);
  };
  const spaces = viewer.spaces;
  const many = showsOwner(spaces);
  const owner = space ?? spaces[0] ?? null;
  const label = workspace ? workspace.name : "Choose a workspace";
  const title = workspace ? pillTitle(workspace.name, owner, viewer, many) : label;
  const searchable = needsFilter(spaces);
  const groups = filterGroups(orderOwners(spaces), viewer, filter);

  return (
    <div ref={box} className="relative min-w-0 shrink">
      <button
        type="button"
        onClick={() => setOpen((o) => !o)}
        aria-haspopup="listbox"
        aria-expanded={open}
        aria-label={`Workspace: ${title}`}
        title={title}
        className="hit inline-flex h-8 max-w-[min(42vw,320px)] min-w-0 items-center gap-1.5 rounded-[6px] border border-rule bg-surface px-2 text-[12.5px] text-ink transition-colors hover:border-action md:px-2.5"
      >
        <Layers size={14} className="shrink-0 text-ink-3" aria-hidden />
        <span className="min-w-0 truncate font-medium">{label}</span>
        {workspace && many && owner && <span className="hidden max-w-[45%] shrink-[2] truncate text-ink-3 sm:inline">{pillOwner(owner)}</span>}
        <ChevronDown size={14} className="shrink-0 text-ink-3" aria-hidden />
      </button>
      {open && (
        <div className="fixed inset-x-2 top-12 z-50 flex max-h-[min(70vh,560px)] flex-col overflow-hidden rounded-[8px] border border-hairline bg-overlay shadow-popover sm:absolute sm:inset-x-auto sm:top-auto sm:right-0 sm:mt-1 sm:w-[320px]">
          {searchable && (
            <div className="border-b border-hairline p-2">
              <label className="flex h-8 items-center gap-2 rounded-[6px] border border-rule bg-surface px-2 text-ink-3 focus-within:border-action focus-within:shadow-[0_0_0_3px_color-mix(in_srgb,var(--lake-focus)_35%,transparent)]">
                <Search size={14} aria-hidden />
                <input
                  autoFocus
                  type="search"
                  value={filter}
                  onChange={(e) => setFilter(e.target.value)}
                  onKeyDown={(e) => {
                    if (e.key === "Enter") {
                      const id = firstMatch(groups);
                      if (id) go(id);
                    }
                  }}
                  placeholder="Filter workspaces"
                  aria-label="Filter workspaces"
                  className="h-full w-full min-w-0 border-0 bg-transparent p-0 text-[13px] text-ink outline-none focus-visible:shadow-none focus-visible:outline-none max-sm:text-[16px]"
                />
              </label>
            </div>
          )}
          <div role="listbox" aria-label="Workspaces" className="min-h-0 flex-1 overflow-y-auto overscroll-contain py-1">
            {groups.map((s) => (
              <Group key={s.id} space={s} viewer={viewer} current={workspaceId} onChoose={go} />
            ))}
            {!groups.length && <div className="px-3 py-3 text-[12.5px] text-ink-3">No workspace matches “{filter.trim()}”.</div>}
          </div>
          <Link
            to={newWorkspacePath(owner?.id)}
            onClick={() => setOpen(false)}
            className="flex items-center gap-2 border-t border-hairline px-3 py-2.5 text-[13px] text-ink-2 no-underline hover:bg-raised hover:text-ink max-md:min-h-[44px]"
          >
            <Plus size={14} aria-hidden />
            <span className="flex-1">New workspace</span>
            {many && <span className="text-[11.5px] text-ink-3">choose its owner</span>}
          </Link>
        </div>
      )}
    </div>
  );
}

function Group({ space, viewer, current, onChoose }: { space: SpaceChoice; viewer: Parameters<typeof ownerName>[1]; current: string | null; onChoose: (id: string) => void }) {
  const name = ownerName(space, viewer);
  return (
    <div role="group" aria-label={`${ownerKind(space)}: ${name}`} className="py-1">
      <div className="flex items-center gap-2 px-3 pt-1 pb-1.5">
        <span
          aria-hidden
          className={cx(
            "inline-flex h-6 w-6 shrink-0 items-center justify-center rounded-[6px] text-[11.5px] font-semibold",
            space.personal ? "bg-ai-soft text-ai" : "bg-action-soft text-action",
          )}
        >
          {initialOf(name)}
        </span>
        <span className="min-w-0 leading-tight">
          <span className="block text-[11px] text-ink-3">{ownerKind(space)}</span>
          <span className="block truncate text-[13px] font-medium text-ink" title={name}>
            {name}
          </span>
        </span>
      </div>
      {space.workspaces.map((w) => (
        <button
          key={w.id}
          type="button"
          role="option"
          aria-selected={w.id === current}
          onClick={() => onChoose(w.id)}
          title={w.name}
          className={cx(
            "flex w-full items-center gap-2 py-1.5 pr-3 pl-11 text-left text-[13px] hover:bg-raised max-md:min-h-[44px]",
            w.id === current ? "font-semibold text-action" : "text-ink",
          )}
        >
          <span className="min-w-0 flex-1 truncate">{w.name}</span>
          {w.id === current && <Check size={14} className="shrink-0" aria-hidden />}
        </button>
      ))}
      {!space.workspaces.length && <div className="py-1.5 pr-3 pl-11 text-[12.5px] text-ink-3">No workspaces yet.</div>}
    </div>
  );
}
