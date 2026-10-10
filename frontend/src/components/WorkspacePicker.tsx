/** The space and workspace picker in the top bar: one pill naming the current workspace, which opens
 * the viewer's spaces with their workspaces. Choosing a workspace keeps the page: a workspace page
 * reopens under the new workspace, any other page goes to its Projects. The choice lives in the URL
 * (`/w/<id>/…`); this browser remembers the last one for unprefixed links. */
import { useEffect, useRef, useState } from "react";
import { Link, useLocation, useNavigate } from "react-router-dom";
import { Check, ChevronDown, Layers, Plus } from "lucide-react";
import { useSession } from "@/lib/session";
import { isWorkspacePath, localPath } from "@/routes";
import { cx } from "@/lib/ui";

export function WorkspacePicker() {
  const { viewer, space, workspace, workspaceId } = useSession();
  const location = useLocation();
  const navigate = useNavigate();
  const [open, setOpen] = useState(false);
  const box = useRef<HTMLDivElement>(null);
  useEffect(() => {
    setOpen(false);
  }, [location.pathname]);
  useEffect(() => {
    if (!open) return;
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
  const label = workspace ? workspace.name : "Choose a workspace";
  const spaceLabel = space?.label ?? viewer.spaces[0]?.label;

  return (
    <div ref={box} className="relative">
      <button
        type="button"
        onClick={() => setOpen((o) => !o)}
        aria-haspopup="listbox"
        aria-expanded={open}
        className="inline-flex h-8 max-w-[260px] items-center gap-1.5 rounded-[6px] border border-rule bg-surface px-2.5 text-[12.5px] text-ink transition-colors hover:border-action"
        title={spaceLabel ? `${spaceLabel} · ${label}` : label}
      >
        <Layers size={14} className="shrink-0 text-ink-3" />
        {spaceLabel && <span className="hidden truncate text-ink-3 lg:inline">{spaceLabel}</span>}
        {spaceLabel && <span className="hidden text-rule lg:inline">/</span>}
        <span className="truncate font-medium">{label}</span>
        <ChevronDown size={14} className="shrink-0 text-ink-3" />
      </button>
      {open && (
        <div role="listbox" className="absolute right-0 z-50 mt-1 max-h-[70vh] w-[280px] overflow-auto rounded-[6px] border border-hairline bg-overlay py-1 shadow-popover">
          {viewer.spaces.map((s) => (
            <div key={s.id} className="py-1">
              <div className="px-3 pt-1 pb-1 font-mono text-[10px] font-semibold tracking-[0.12em] text-ink-3 uppercase">{s.label}</div>
              {s.workspaces.map((w) => (
                <button
                  key={w.id}
                  type="button"
                  role="option"
                  aria-selected={w.id === workspaceId}
                  onClick={() => go(w.id)}
                  className={cx(
                    "flex w-full items-center gap-2 px-3 py-1.5 text-left text-[13px] hover:bg-raised",
                    w.id === workspaceId ? "font-semibold text-action" : "text-ink",
                  )}
                >
                  <span className="flex-1 truncate">{w.name}</span>
                  {w.id === workspaceId && <Check size={14} />}
                </button>
              ))}
              {!s.workspaces.length && <div className="px-3 py-1.5 text-[12.5px] text-ink-3">No workspaces yet.</div>}
              <Link
                to={`/workspaces?customer_id=${encodeURIComponent(s.id)}`}
                className="flex items-center gap-2 px-3 py-1.5 text-[12.5px] text-ink-3 no-underline hover:bg-raised hover:text-ink"
              >
                <Plus size={13} /> New workspace
              </Link>
            </div>
          ))}
        </div>
      )}
    </div>
  );
}
