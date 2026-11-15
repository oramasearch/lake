// SPDX-License-Identifier: MIT

/** New drive (create_drive), in a drawer: its name, how it keeps its files, for a repository
 * drive a new repository (named from the drive) or one this workspace already has, and for a
 * storage drive one of the workspace's buckets or filesystems (lake#656). Drives → Storage opens it
 * with that storage chosen. */
import { useState } from "react";
import { act, useRead, type Row } from "@/lib/api";
import { Drawer } from "@/lib/overlay";
import { Button, Field, FormError, cx, fieldError } from "@/lib/ui";
import { useWorkspace } from "@/pages/train/workspace";
import type { Repository } from "@/pages/repositories/forge";
import { driveSlug, validName } from "./paths";
import { STORAGE, type Storage } from "./targets";
import type { Drive } from "./storage";
import { browsable } from "./stores";

export function NewDrive({ onClose, onCreated, store }: { onClose: () => void; onCreated: (drive: Drive) => void; store?: Row }) {
  const { customerId, workspaceId } = useWorkspace();
  const [name, setName] = useState(store ? String(store.name ?? "") : "");
  const [storage, setStorage] = useState<Storage>(store ? "store" : "files");
  const [storeId, setStoreId] = useState(store ? String(store.id ?? "") : "");
  const stores = useRead<{ stores?: Row[] }>(storage === "store" && customerId ? "list_stores" : null, { customer_id: customerId, workspace_id: workspaceId });
  const storeList = (stores.data?.stores ?? []).filter((s) => s.desired !== "absent");
  const openable = storeList.filter((s) => browsable(s) === "yes");
  const [source, setSource] = useState<"new" | "existing">("new");
  const [slug, setSlug] = useState("");
  const [slugTouched, setSlugTouched] = useState(false);
  const [repositoryId, setRepositoryId] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const repositories = useRead<{ items: Repository[] }>(storage === "repository" && customerId ? "list_repositories" : null, { customer_id: customerId, limit: 500 }, { workspace: null });
  const here = (repositories.data?.items ?? []).filter((r) => r.workspace_id === workspaceId && !r.deleted_at);
  const repositoryName = slugTouched ? slug : driveSlug(name);
  const nameProblem = name && !validName(name.trim()) ? "A name has no slash or backslash and is at most 255 bytes." : undefined;

  const submit = async () => {
    setBusy(true);
    setError(null);
    try {
      const input: Row = { customer_id: customerId, workspace_id: workspaceId, name: name.trim(), storage };
      if (storage === "repository") {
        if (source === "existing") input.repository_id = repositoryId;
        else input.repository_name = repositoryName;
      }
      if (storage === "store") input.store_id = storeId;
      onCreated(await act<Drive>("create_drive", input));
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  const ready =
    !!name.trim() && !nameProblem && (storage !== "repository" || (source === "new" ? !!repositoryName : !!repositoryId)) && (storage !== "store" || !!storeId);

  return (
    <Drawer
      title="New Drive"
      subtitle="A place for this workspace's files."
      onClose={onClose}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button type="submit" form="new-drive" variant="primary" loading={busy} disabled={!ready}>
            Create drive
          </Button>
        </>
      }
    >
      <form
        id="new-drive"
        className="stack"
        onSubmit={(e) => {
          e.preventDefault();
          if (ready) void submit();
        }}
      >
        {!["name", "repository_name", "repository_id", "store_id"].some((field) => fieldError(error, field)) && <FormError error={error} />}
        <Field label="Name" required error={nameProblem ?? fieldError(error, "name")}>
          <input value={name} onChange={(e) => setName(e.target.value)} placeholder="Team files" autoFocus />
        </Field>
        <fieldset className="m-0 border-0 p-0">
          <legend className="label mb-2">Keeps its files as</legend>
          <div className="grid gap-2">
            {STORAGE.map((option) => (
              <label
                key={option.id}
                className={cx(
                  "flex cursor-pointer gap-3 rounded-[6px] border p-3",
                  storage === option.id ? "border-action bg-action-soft" : "border-hairline bg-surface hover:border-rule",
                )}
              >
                <input type="radio" name="storage" value={option.id} checked={storage === option.id} onChange={() => setStorage(option.id)} className="mt-1" />
                <span className="min-w-0">
                  <span className="block text-[13.5px] font-semibold text-ink">{option.label}</span>
                  <span className="block text-[12.5px] text-ink-2">{option.detail}</span>
                </span>
              </label>
            ))}
          </div>
        </fieldset>
        {storage === "store" && (
          <Field
            label="Storage"
            help="Only a bucket or filesystem opens as a drive; a volume belongs to the service that mounts it. Nothing is copied, and deleting the drive never deletes the storage."
            error={fieldError(error, "store_id")}
          >
            <select value={storeId} onChange={(e) => setStoreId(e.target.value)}>
              <option value="">{stores.isLoading ? "Loading storage…" : openable.length ? "Choose storage…" : "No bucket or filesystem is ready"}</option>
              {storeList.map((s) => {
                const kind = browsable(s);
                return (
                  <option key={String(s.id)} value={String(s.id)} disabled={kind !== "yes"}>
                    {String(s.name)}
                    {kind === "volume" ? " (volume: shown, not browsed)" : kind === "not_ready" ? " (not ready)" : kind === "other" ? " (not browsable)" : ""}
                  </option>
                );
              })}
            </select>
          </Field>
        )}
        {storage === "repository" && (
          <fieldset className="m-0 border-0 p-0">
            <legend className="label mb-2">Repository</legend>
            <div className="flex flex-wrap gap-4 text-[13px]">
              <label className="flex items-center gap-2">
                <input type="radio" name="source" checked={source === "new"} onChange={() => setSource("new")} /> Create a new one
              </label>
              <label className="flex items-center gap-2">
                <input type="radio" name="source" checked={source === "existing"} onChange={() => setSource("existing")} disabled={!here.length} /> Use one of this workspace
              </label>
            </div>
            {source === "new" ? (
              <Field label="Repository name" help="Lowercase letters, numbers and hyphens." error={fieldError(error, "repository_name")}>
                <input
                  value={repositoryName}
                  onChange={(e) => {
                    setSlugTouched(true);
                    setSlug(e.target.value);
                  }}
                  placeholder="team-files"
                />
              </Field>
            ) : (
              <Field label="Repository" help="Deleting the drive later never deletes the repository." error={fieldError(error, "repository_id")}>
                <select value={repositoryId} onChange={(e) => setRepositoryId(e.target.value)}>
                  <option value="">Choose a repository…</option>
                  {here.map((r) => (
                    <option key={r.id} value={r.id}>
                      {r.name}
                    </option>
                  ))}
                </select>
              </Field>
            )}
          </fieldset>
        )}
      </form>
    </Drawer>
  );
}
