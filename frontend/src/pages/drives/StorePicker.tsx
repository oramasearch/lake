/** Choose the workspace storage a service or an app mounts (lake#666, docs/ui/drives.md#mounts):
 * one row per store of the workspace, a volume with the path it is mounted at, a bucket as
 * environment variables, each read-only or not. A store that cannot be mounted says why (a volume
 * another service or app mounts, a store Lake keeps). Used by New service, a service's Storage
 * dialog on Ship → Services, and the app's Services tab. */
import { Link } from "react-router-dom";
import type { Row } from "@/lib/api";
import { Skeleton } from "@/lib/ui";
import { text } from "@/pages/train/workspace";
import { defaultMountPath, mountProblem, storageHref, type MountRequest } from "./stores";

export function StorePicker({
  stores,
  value,
  onChange,
  owner,
  workspaceId,
  loading,
}: {
  stores: Row[];
  value: MountRequest[];
  onChange: (next: MountRequest[]) => void;
  /** The workload choosing: its kind, and its id once it exists. */
  owner: { kind: "service" | "app"; id?: string };
  workspaceId: string;
  loading?: boolean;
}) {
  if (loading) return <Skeleton lines={2} />;
  const live = stores.filter((s) => text(s, "desired") !== "absent");
  if (live.length === 0) {
    return (
      <p className="muted m-0 text-[12.5px]">
        No storage in this workspace yet. Create a volume on <Link to={storageHref(workspaceId)}>Data → Drives → Storage</Link>.
      </p>
    );
  }
  const set = (id: string, change: Partial<MountRequest>) => onChange(value.map((m) => (m.store_id === id ? { ...m, ...change } : m)));
  return (
    <ul className="m-0 list-none space-y-2 p-0" aria-label="Storage to mount">
      {live.map((store) => {
        const id = text(store, "id");
        const name = text(store, "name");
        const volume = text(store, "type") === "volume";
        const chosen = value.find((m) => m.store_id === id);
        const problem = mountProblem(store, owner);
        const envName = `STORE_${name.toUpperCase().replaceAll("-", "_")}_…`;
        return (
          <li key={id} className="rounded-[5px] border border-hairline px-3 py-2">
            <label className="flex items-start gap-2 text-[13px]">
              <input
                type="checkbox"
                className="mt-1"
                checked={!!chosen}
                disabled={!chosen && !!problem}
                onChange={(e) =>
                  onChange(e.target.checked ? [...value, { store_id: id, ...(volume ? { mount_path: defaultMountPath(name) } : {}) }] : value.filter((m) => m.store_id !== id))
                }
              />
              <span className="min-w-0">
                <strong className="text-ink">{name}</strong>{" "}
                <span className="text-[12px] text-ink-3">
                  {volume ? "Volume" : "Bucket"}
                  {!chosen && problem ? ` · ${problem}` : ""}
                </span>
              </span>
            </label>
            {chosen && (
              <div className="mt-2 grid grid-cols-1 items-end gap-2 sm:grid-cols-[minmax(0,1fr)_auto]">
                {volume ? (
                  <label className="text-[12.5px]">
                    Mounted at
                    <input
                      name={`mount_path_${id}`}
                      required
                      pattern="/[A-Za-z0-9._/\-]{1,255}"
                      value={chosen.mount_path ?? ""}
                      onChange={(e) => set(id, { mount_path: e.target.value })}
                      aria-label={`Path ${name} is mounted at`}
                    />
                  </label>
                ) : (
                  <span className="text-[12px] text-ink-3">
                    As environment variables <code>{envName}</code>
                  </span>
                )}
                <label className="flex items-center gap-1.5 text-[12.5px]">
                  <input type="checkbox" checked={!!chosen.read_only} onChange={(e) => set(id, { read_only: e.target.checked })} /> Read-only
                </label>
              </div>
            )}
          </li>
        );
      })}
    </ul>
  );
}
