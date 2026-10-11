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
