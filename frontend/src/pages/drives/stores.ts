/** Data → Drives → Storage (lake#656, docs/ui/drives.md#storage): the words and figures for the
 * workspace's stores (buckets, filesystems and volumes; `list_stores` in the API). Pure, so
 * `tests/drives.test.ts` runs it under Node. The page says "storage"; the API says "store". */

export type StoreRow = Record<string, unknown>;

function str(row: StoreRow | null | undefined, key: string): string {
  const value = row?.[key];
  return value === null || value === undefined ? "" : String(value);
}

/** What a person reads for the store's state: the operator's phase, Deleting once withdrawn,
 * Connecting while a Ready store's connection reaches Lake, and Waiting before the operator. */
export function storePhase(store: StoreRow): string {
  const phase = str(store, "phase");
  if (str(store, "desired") === "absent") return !phase || phase === "Ready" ? "Deleting" : phase;
  if (phase === "Ready" && store.connected === false) return "Connecting";
  return phase || "Waiting for the operator";
}

export function storeSettled(store: StoreRow): boolean {
  return ["Ready", "Failed"].includes(storePhase(store));
}

export function phaseTone(phase: string): "success" | "danger" | "warning" {
  return phase === "Ready" ? "success" : phase === "Failed" ? "danger" : "warning";
}

/** "Volume" and "cluster disk" from the provider label "Volume — cluster disk". */
export function storeKindWords(providers: StoreRow[], type: string, provider: string): [string, string] {
  const known = providers.find((p) => str(p, "type") === type && str(p, "provider") === provider);
  if (!known) return [type, provider];
  const label = str(known, "label");
  const at = label.indexOf(" — ");
  return at < 0 ? [label, provider] : [label.slice(0, at), label.slice(at + 3)];
}

/** A volume is shown, not browsed: its contents belong to the service that mounts it. A bucket or
 * filesystem opens as a drive once it is Ready and its connection reached Lake. */
export function browsable(store: StoreRow): "yes" | "volume" | "not_ready" | "other" {
  const type = str(store, "type");
  if (type === "volume") return "volume";
  if (type !== "object" && type !== "filesystem") return "other";
  return storePhase(store) === "Ready" ? "yes" : "not_ready";
}

export type StoreFigures = { count: number; changing: number; browsable: number; volumes: number; sizeGi: number };

/** The Storage tab's figures: every live store, how many are changing, how many open as drives,
 * the volumes, and the size they ask for in GiB. */
export function storeFigures(stores: StoreRow[]): StoreFigures {
  const live = stores.filter((s) => str(s, "desired") !== "absent");
  return {
    count: stores.length,
    changing: stores.filter((s) => !storeSettled(s)).length,
    browsable: live.filter((s) => browsable(s) === "yes").length,
    volumes: live.filter((s) => str(s, "type") === "volume").length,
    sizeGi: live.reduce((sum, s) => sum + (Number(s.size_gi) || 0), 0),
  };
}

/** The drive that already opens a store, so its row links there instead of offering another. */
export function driveForStore<T extends { store_id?: string | null }>(drives: T[], storeId: string): T | undefined {
  return drives.find((d) => d.store_id === storeId);
}

/* ------------------------------------------------------------------ mounts (lake#666) */

/** One service or app that mounts a store: `list_stores`' `mounted_by`. */
export type MountedBy = { kind: string; id: string; name: string; mount_path: string; read_only: boolean };

/** What a service or an app asks to mount: the `stores` input of create_service,
 * set_service_stores, create_app and set_app_stores. */
export type MountRequest = { store_id: string; mount_path?: string; read_only?: boolean };

export function mountedBy(store: StoreRow): MountedBy[] {
  const value = store.mounted_by;
  if (!Array.isArray(value)) return [];
  return value
    .filter((m): m is Record<string, unknown> => !!m && typeof m === "object")
    .map((m) => ({ kind: str(m, "kind"), id: str(m, "id"), name: str(m, "name"), mount_path: str(m, "mount_path"), read_only: m.read_only === true }));
}

function query(values: Record<string, string>): string {
  return Object.entries(values)
    .filter(([, v]) => v !== "")
    .map(([k, v]) => `${encodeURIComponent(k)}=${encodeURIComponent(v)}`)
    .join("&");
}

/** Where a mount's workload opens: an app's page in the store's workspace; a service on Ship →
 * Services (the account's services, one page), searched for its name. */
export function workloadHref(mount: MountedBy, customerId: string, workspaceId: string): string {
  if (mount.kind === "app") return `/w/${encodeURIComponent(workspaceId)}/ship/app?${query({ customer_id: customerId, app_id: mount.id })}`;
  return `/services?${query({ customer_id: customerId, q: mount.name })}`;
}

/** Where a workload's storage opens: Data → Drives → Storage of its workspace. */
export function storageHref(workspaceId: string): string {
  return `/w/${encodeURIComponent(workspaceId)}/drives?tab=storage`;
}

/** Bytes in words: "512 B", "3.0 MiB", "1.5 GiB". */
export function bytesWords(bytes: number): string {
  const units = ["B", "KiB", "MiB", "GiB", "TiB"];
  let value = bytes;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit += 1;
  }
  return unit === 0 ? `${value} B` : `${value.toFixed(1)} ${units[unit]}`;
}

/** A store's used space beside the size asked for: "3.0 MiB used" (with the share of a volume's
 * size), or why there is no figure yet. The operator measures a volume once a pod mounts it. */
export function usedWords(store: StoreRow): string {
  const used = store.used_bytes;
  if (typeof used !== "number") return str(store, "type") === "volume" && mountedBy(store).length === 0 ? "measured once mounted" : "not measured yet";
  const size = Number(store.size_gi) || 0;
  const share = str(store, "type") === "volume" && size > 0 ? Math.round((used / (size * 1024 ** 3)) * 100) : null;
  return share === null ? `${bytesWords(used)} used` : `${bytesWords(used)} used (${share}%)`;
}

/** Whether a workload may mount a store, and why not: a store being deleted, one Lake keeps
 * (zetta, lake), a kind not mountable yet, or a volume another workload mounts (a volume is
 * mounted by one service or app). `owner` is the workload asking (kind and id; no id yet while
 * it is being created). */
export function mountProblem(store: StoreRow, owner: { kind: string; id?: string }): string {
  if (str(store, "desired") === "absent") return "being deleted";
  const provider = str(store, "provider");
  if (provider === "zetta" || provider === "lake") return "kept by Lake; open it as a drive";
  const type = str(store, "type");
  if (type !== "volume" && !(type === "object" && ["r2", "s3", "b2", "gcs", "azure"].includes(provider))) return "cannot be mounted yet";
  if (type === "volume") {
    const other = mountedBy(store).find((m) => !(m.kind === owner.kind && m.id === owner.id));
    if (other) return `mounted by the ${other.kind} ${other.name}`;
  }
  return "";
}

/** The path a volume is offered at before the person changes it. */
export function defaultMountPath(name: string): string {
  return `/mnt/${name}`;
}

/** The `stores` input for a choice: a bucket takes no path. */
export function mountInput(choice: MountRequest[], stores: StoreRow[]): MountRequest[] {
  return choice.map((m) => {
    const store = stores.find((s) => str(s, "id") === m.store_id);
    const volume = str(store, "type") === "volume";
    return { store_id: m.store_id, ...(volume ? { mount_path: m.mount_path ?? "" } : {}), ...(m.read_only ? { read_only: true } : {}) };
  });
}
