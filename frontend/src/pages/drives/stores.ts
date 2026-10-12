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

/** What New storage asks for beside the name (lake#664): a volume's size, your own bucket's
 * location and the workspace secret that opens it, a liquid repository for a `lake` store, or
 * nothing for a store Lake keeps in the workspace Zetta. */
export function storeNeeds(type: string, provider: string): "size" | "bucket" | "repository" | "none" {
  if (type === "volume") return "size";
  if (type === "object" && ["r2", "s3", "b2", "gcs", "azure"].includes(provider)) return "bucket";
  if (provider === "lake") return "repository";
  return "none";
}

/** The endpoint a bucket form suggests, and whether it must be given (R2's account, Azure's
 * account; B2 without a region). */
export function endpointHint(provider: string): { placeholder: string; required: boolean; help: string } {
  switch (provider) {
    case "r2":
      return { placeholder: "https://<account id>.r2.cloudflarestorage.com", required: true, help: "Your account's S3 API endpoint." };
    case "azure":
      return { placeholder: "https://<account>.blob.core.windows.net", required: true, help: "Your storage account's Blob endpoint." };
    case "b2":
      return { placeholder: "https://s3.<region>.backblazeb2.com", required: false, help: "Or leave it empty and give the region." };
    case "gcs":
      return { placeholder: "https://storage.googleapis.com", required: false, help: "Empty: Google Cloud Storage's XML API." };
    default:
      return { placeholder: "https://s3.<region>.amazonaws.com", required: false, help: "Empty: AWS S3 in the region below." };
  }
}

/** The fields the workspace secret must hold for a bucket of this provider. */
export function credentialFields(provider: string): string {
  return provider === "azure" ? "account_key, or sas_token" : "access_key_id and secret_access_key";
}

/** What deleting a store does to what is in it: a volume's and a workspace Zetta store's data
 * go with it; your own bucket and a `lake` store's repository stay. */
export function deleteWarning(store: StoreRow): { tone: "warn" | "quiet"; text: string } {
  const type = str(store, "type");
  const provider = str(store, "provider");
  if (storeNeeds(type, provider) === "bucket") {
    return { tone: "quiet", text: "Lake forgets this bucket. The bucket and what is in it stay in your account. A drive that opens it stops working; delete that drive too." };
  }
  if (provider === "lake") {
    return { tone: "quiet", text: "Lake forgets this storage. Its repository stays, with its history. A drive that opens it stops working; delete that drive too." };
  }
  return { tone: "warn", text: "Deleting storage deletes the data in it, which cannot be recovered. A drive that opens it stops working; delete that drive too." };
}

/** The size column: a volume's GiB asked for; nothing to ask for otherwise. */
export function storeSizeText(store: StoreRow): string {
  return str(store, "type") === "volume" ? `${str(store, "size_gi")} GiB` : "—";
}

/** Your own bucket's name and folder, shown under the storage's name. */
export function storeWhere(store: StoreRow): string {
  const location = (store.location ?? null) as StoreRow | null;
  if (location && str(location, "bucket")) {
    const prefix = str(location, "prefix");
    return `${str(location, "bucket")}${prefix ? "/" + prefix : ""}`;
  }
  return "";
}
