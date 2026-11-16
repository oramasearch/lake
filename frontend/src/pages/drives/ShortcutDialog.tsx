// SPDX-License-Identifier: MIT

/** New shortcut (create_drive_shortcut): a name and what it opens. Repositories, transcripts and
 * drive items are chosen from what the workspace has; a dataset, project, agent or model is named;
 * a web address is typed. */
import { useMemo, useRef, useState } from "react";
import { useRead } from "@/lib/api";
import { Modal } from "@/lib/overlay";
import { useToast } from "@/lib/toast";
import { refreshAfterWrite } from "@/lib/api";
import { Button, Field, FormError, fieldError } from "@/lib/ui";
import type { Repository } from "@/pages/repositories/forge";
import { useWorkspace } from "@/pages/train/workspace";
import { validName } from "./paths";
import { storageFor, type Drive, type DriveStorage, type Item } from "./storage";
import { TARGETS, cleanTarget, nameAfterTarget, selectsOnFocus, targetProblem, type Target, type TargetKind } from "./targets";
import { FolderPicker } from "./FolderPicker";

export function ShortcutDialog({ storage, folder, onClose }: { storage: DriveStorage; folder: string; onClose: () => void }) {
  const toast = useToast();
  const { customerId, workspaceId } = useWorkspace();
  const [kind, setKind] = useState<TargetKind>("repository");
  const [target, setTarget] = useState<Target>({ kind: "repository" });
  const [name, setName] = useState("");
  const [named, setNamed] = useState(false);
  const selectedOnFocus = useRef(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const [picking, setPicking] = useState<Drive | null>(null);
  const [pickedLabel, setPickedLabel] = useState("");
  const scope = { customer_id: customerId, workspace_id: workspaceId };
  const needsRepository = kind === "repository" || kind === "repository_path";
  const repositories = useRead<{ items: Repository[] }>(needsRepository && customerId ? "list_repositories" : null, { customer_id: customerId, limit: 500 }, { workspace: null });
  const transcripts = useRead<{ transcripts: { id: string; title: string }[] }>(kind === "transcript" && customerId ? "list_transcripts" : null, { ...scope, limit: 100 });
  const drives = useRead<{ drives: Drive[] }>(kind === "drive_entry" && customerId ? "list_drives" : null, scope);
  const here = (repositories.data?.items ?? []).filter((r) => r.workspace_id === workspaceId && !r.deleted_at);
  const pickable = (drives.data?.drives ?? []).filter((d) => d.storage !== "repository");
  const pickingStorage = useMemo(() => (picking ? storageFor(picking, { customerId, workspaceId }) : null), [picking, customerId, workspaceId]);

  const set = (patch: Partial<Target>, suggested?: string) => {
    setTarget((current) => ({ ...current, ...patch }));
    if (suggested) setName((current) => nameAfterTarget(current, named, suggested));
  };
  const choose = (next: TargetKind) => {
    setKind(next);
    setTarget({ kind: next });
    setPickedLabel("");
    if (!named) setName("");
  };
  const problem = targetProblem({ ...target, kind });
  const nameProblem = name && !validName(name.trim()) ? "A name has no slash or backslash and is at most 255 bytes." : undefined;
  const ready = !problem && !!name.trim() && !nameProblem;

  return (
    <>
      <Modal
        title="New Shortcut"
        subtitle="A link to something that lives elsewhere. Deleting the shortcut never deletes what it opens."
        onClose={onClose}
        width={540}
        footer={
          <>
            <Button onClick={onClose}>Cancel</Button>
            <Button type="submit" form="drive-new-shortcut" variant="primary" loading={busy} disabled={!ready}>
              Create shortcut
            </Button>
          </>
        }
      >
        <form
          id="drive-new-shortcut"
          className="stack"
          onSubmit={async (e) => {
            e.preventDefault();
            if (!ready) return;
            setBusy(true);
            setError(null);
            try {
              await storage.createShortcut(folder, name.trim(), cleanTarget({ ...target, kind }));
              refreshAfterWrite();
              toast("Shortcut created");
              onClose();
            } catch (err) {
              setError(err);
            } finally {
              setBusy(false);
            }
          }}
        >
          <FormError error={error} />
          <Field label="Opens" help={TARGETS.find((t) => t.kind === kind)?.help}>
            <select value={kind} onChange={(e) => choose(e.target.value as TargetKind)}>
              {TARGETS.map((t) => (
                <option key={t.kind} value={t.kind}>
                  {t.label}
                </option>
              ))}
            </select>
          </Field>
          {needsRepository && (
            <Field label="Repository" required error={fieldError(error, "target")}>
              <select
                value={target.repository_id ?? ""}
                onChange={(e) => set({ repository_id: e.target.value }, kind === "repository" ? here.find((r) => r.id === e.target.value)?.name : undefined)}
              >
                <option value="">{repositories.isLoading ? "Loading…" : here.length ? "Choose a repository…" : "This workspace has no repository"}</option>
                {here.map((r) => (
                  <option key={r.id} value={r.id}>
                    {r.name}
                  </option>
                ))}
              </select>
            </Field>
          )}
          {kind === "repository_path" && (
            <Field label="Path" required help="From the repository's top, such as reports/2026/q3.pdf.">
              <input value={target.path ?? ""} onChange={(e) => set({ path: e.target.value }, e.target.value.split("/").filter(Boolean).pop())} placeholder="reports/2026/q3.pdf" />
            </Field>
          )}
          {kind === "transcript" && (
            <Field label="Transcript" required>
              <select value={target.id ?? ""} onChange={(e) => set({ id: e.target.value }, transcripts.data?.transcripts.find((t) => t.id === e.target.value)?.title)}>
                <option value="">{transcripts.isLoading ? "Loading…" : (transcripts.data?.transcripts.length ?? 0) ? "Choose a transcript…" : "This workspace has no transcript"}</option>
                {(transcripts.data?.transcripts ?? []).map((t) => (
                  <option key={t.id} value={t.id}>
                    {t.title}
                  </option>
                ))}
              </select>
            </Field>
          )}
          {kind === "drive_entry" && (
            <Field label="Drive" required help={pickedLabel ? `Chosen: ${pickedLabel}` : "Choose the drive, then the item in it."}>
              <select
                value={target.drive_id ?? ""}
                onChange={(e) => {
                  const drive = pickable.find((d) => d.id === e.target.value) ?? null;
                  set({ drive_id: e.target.value, entry_id: "" });
                  setPickedLabel("");
                  if (drive) setPicking(drive);
                }}
              >
                <option value="">{drives.isLoading ? "Loading…" : pickable.length ? "Choose a drive…" : "No workspace-files or virtual drive yet"}</option>
                {pickable.map((d) => (
                  <option key={d.id} value={d.id}>
                    {d.name}
                  </option>
                ))}
              </select>
            </Field>
          )}
          {kind === "drive_entry" && target.drive_id && (
            <div>
              <Button onClick={() => setPicking(pickable.find((d) => d.id === target.drive_id) ?? null)}>{pickedLabel ? "Choose another item" : "Choose the item"}</Button>
            </div>
          )}
          {(kind === "dataset" || kind === "agent" || kind === "model") && (
            <Field label={kind === "dataset" ? "Dataset" : kind === "agent" ? "Agent id" : "Model id"} required>
              <input value={target.id ?? ""} onChange={(e) => set({ id: e.target.value }, e.target.value.split("/").pop())} placeholder={kind === "dataset" ? "datasets/orders" : ""} />
            </Field>
          )}
          {kind === "project" && (
            <Field label="Project" required>
              <input value={target.name ?? ""} onChange={(e) => set({ name: e.target.value }, e.target.value)} placeholder="churn" />
            </Field>
          )}
          {kind === "url" && (
            <Field label="Web address" required>
              <input type="url" value={target.url ?? ""} onChange={(e) => set({ url: e.target.value })} placeholder="https://" />
            </Field>
          )}
          <Field label="Name" required error={nameProblem ?? fieldError(error, "name")} help="How the shortcut is listed in this folder.">
            <input
              value={name}
              // A suggested name is selected on focus, so typing replaces it (lake#580). A click
              // would move the caret on mouse-up and drop the selection, so that one mouse-up is
              // let pass.
              onFocus={(e) => {
                if (selectsOnFocus(name, named)) {
                  e.currentTarget.select();
                  selectedOnFocus.current = true;
                }
              }}
              onMouseUp={(e) => {
                if (selectedOnFocus.current) {
                  e.preventDefault();
                  selectedOnFocus.current = false;
                }
              }}
              onChange={(e) => {
                setNamed(true);
                setName(e.target.value);
              }}
            />
          </Field>
          {problem && target.kind === kind && Object.keys(target).length > 1 && <p className="muted m-0 text-[12.5px]">{problem}</p>}
        </form>
      </Modal>
      {picking && pickingStorage && (
        <FolderPicker
          storage={pickingStorage}
          scope={{ customerId, workspaceId }}
          start=""
          items
          title={`Choose an Item in ${picking.name}`}
          action="Choose"
          onClose={() => setPicking(null)}
          onPick={(key, item?: Item) => {
            set({ drive_id: picking.id, entry_id: key }, item?.name);
            setPickedLabel(item ? `${picking.name} / ${item.path || item.name}` : key);
            setPicking(null);
          }}
        />
      )}
    </>
  );
}
