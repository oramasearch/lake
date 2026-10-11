/** The workspace multi-select of Home's Dashboard and Usage: "All workspaces" and one toggle per
 * workspace, kept in `?ws=` and remembered in this browser (lib/workspaceSelection.ts). */
import { useMemo } from "react";
import { useSearchParams } from "react-router-dom";
import { cx } from "@/lib/ui";
import type { WorkspaceChoice } from "@/lib/session";
import { chosenWorkspaceIds, selectionToKeep, toggleWorkspace } from "@/lib/workspaceSelection";

export type WorkspaceSelection = {
  chosen: WorkspaceChoice[];
  chosenIds: string[];
  all: boolean;
  choose: (ids: string[]) => void;
  toggle: (id: string) => void;
};

/** The selection over `workspaces`, remembered under `storeKey`. Other query parameters are kept. */
export function useWorkspaceSelection(workspaces: WorkspaceChoice[], storeKey: string): WorkspaceSelection {
  const [params, setParams] = useSearchParams();
  const known = useMemo(() => workspaces.map((w) => w.id), [workspaces]);
  const chosenIds = useMemo(() => {
    let stored: unknown = null;
    try {
      stored = JSON.parse(localStorage.getItem(storeKey) ?? "null");
    } catch {
      /* storage blocked */
    }
    return chosenWorkspaceIds(params.get("ws"), stored, known);
  }, [params, known, storeKey]);
  const all = chosenIds.length === 0 || chosenIds.length === workspaces.length;
  const chosen = all ? workspaces : workspaces.filter((w) => chosenIds.includes(w.id));
  const choose = (ids: string[]) => {
    const next = selectionToKeep(ids, known);
    try {
      localStorage.setItem(storeKey, JSON.stringify(next));
    } catch {
      /* storage blocked */
    }
    const nextParams = new URLSearchParams(params);
    if (next.length) nextParams.set("ws", next.join(","));
    else nextParams.delete("ws");
    setParams(nextParams, { replace: true });
  };
  const toggle = (id: string) => choose(toggleWorkspace(chosenIds, all, id));
  return { chosen, chosenIds, all, choose, toggle };
}

export function WorkspaceToggles({ workspaces, selection, label = "Workspaces" }: { workspaces: WorkspaceChoice[]; selection: WorkspaceSelection; label?: string }) {
  return (
    <fieldset className="m-0 mb-4 border-0 p-0">
      <legend className="label mb-2">{label}</legend>
      <div className="flex flex-wrap items-center gap-1.5">
        <WorkspaceToggle on={selection.all} onChange={() => selection.choose([])} label="All workspaces" count={workspaces.length} />
        {workspaces.map((w) => (
          <WorkspaceToggle key={w.id} on={!selection.all && selection.chosenIds.includes(w.id)} onChange={() => selection.toggle(w.id)} label={w.name} />
        ))}
      </div>
    </fieldset>
  );
}

export function WorkspaceToggle({ on, onChange, label, count }: { on: boolean; onChange: () => void; label: string; count?: number }) {
  return (
    <label
      className={cx(
        "inline-flex cursor-pointer items-center gap-1.5 rounded-full border px-3 py-1 text-[12.5px] transition-colors select-none",
        on ? "border-action bg-action-soft text-action" : "border-rule bg-surface text-ink-2 hover:border-action",
      )}
    >
      <input type="checkbox" className="sr-only" checked={on} onChange={onChange} />
      <span aria-hidden className={cx("inline-flex h-3.5 w-3.5 items-center justify-center rounded-[3px] border text-[10px]", on ? "border-action bg-action text-on-action" : "border-rule")}>
        {on ? "✓" : ""}
      </span>
      {label}
      {count !== undefined && <span className="font-mono text-[10.5px] opacity-80">{count}</span>}
    </label>
  );
}
