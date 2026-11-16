// SPDX-License-Identifier: MIT

/** Edit a repository file and commit it (lake#1239, docs/ui/drive.md#editing-a-file). The file
 * is read from `repository_file`'s `content_base64`, edited in a monospace textarea (the console
 * ships no code editor component), and committed with `commit_repository_files` as the person, its
 * opened blob sent as `expected_blob`: a file changed on main since it was opened is refused with
 * `conflict` and shown as a diff, never overwritten. A file inside a runtime App's source folder
 * also offers *Commit and redeploy* (`deploy_app` at the new revision), and follows the rollout. */
import { useEffect, useMemo, useState } from "react";
import { Link } from "react-router-dom";
import { Loader2, RefreshCw, Rocket, Save, X } from "lucide-react";
import { act, read, useRead, type Row } from "@/lib/api";
import { Button, Card, Notice, Skeleton, Status } from "@/lib/ui";
import { workspacePath } from "@/routes";
import { DriveDiff, forgeAct, UNSCOPED, type Repository } from "./forge";
import { appsForFile, type AppSource, commitInput, defaultMessage, editableFile, isConflict, type LineEnding, lineDiff, redeployInput, rolloutState } from "./edit";

type FileRead = { path: string; blob: string; size: number; content_base64?: string };
type Opened = { text: string; eol: LineEnding; blob: string };
type Rollout = { app: AppSource; generation: number; state: "rolling" | "ready" | "failed"; message?: string };

async function openFile(repositoryId: string, path: string): Promise<Opened> {
  const file = await read<FileRead>("repository_file", { id: repositoryId, ref: "main", path }, UNSCOPED);
  const editable = editableFile(file.content_base64, Number(file.size) || 0);
  if (!editable.ok) throw new Error(editable.reason);
  return { text: editable.text, eol: editable.eol, blob: file.blob };
}

export function FileEditor({ repository, path, onClose, onCommitted }: { repository: Repository; path: string; onClose: () => void; onCommitted: () => void }) {
  const [opened, setOpened] = useState<Opened | null>(null);
  const [openError, setOpenError] = useState<string | null>(null);
  const [text, setText] = useState("");
  const [message, setMessage] = useState("");
  const [busy, setBusy] = useState<"commit" | "redeploy" | null>(null);
  const [error, setError] = useState<unknown>(null);
  const [conflict, setConflict] = useState<Opened | null>(null);
  const [committed, setCommitted] = useState<string | null>(null);
  const [rollouts, setRollouts] = useState<Rollout[]>([]);

  useEffect(() => {
    let live = true;
    openFile(repository.id, path).then(
      (file) => {
        if (!live) return;
        setOpened(file);
        setText(file.text);
      },
      (e: unknown) => live && setOpenError(e instanceof Error ? e.message : String(e)),
    );
    return () => {
      live = false;
    };
  }, [repository.id, path]);

  const workspace = repository.workspace_id ?? null;
  const apps = useRead<{ apps?: AppSource[] }>(workspace ? "list_apps" : null, { customer_id: repository.customer_id, workspace_id: workspace ?? undefined }, { workspace });
  const owners = useMemo(() => appsForFile(apps.data?.apps ?? [], repository.id, path), [apps.data, repository.id, path]);

  // Follow each redeployed App until it is Ready at the new generation, or fails.
  useEffect(() => {
    if (!rollouts.some((rollout) => rollout.state === "rolling")) return;
    const timer = window.setTimeout(async () => {
      const next = await Promise.all(
        rollouts.map(async (rollout) => {
          if (rollout.state !== "rolling") return rollout;
          try {
            const app = await read<Row>("get_app", { customer_id: repository.customer_id, workspace_id: workspace ?? undefined, app_id: rollout.app.id }, { workspace });
            return { ...rollout, state: rolloutState(app, rollout.generation), message: typeof app.message === "string" ? app.message : undefined };
          } catch {
            return rollout;
          }
        }),
      );
      setRollouts(next);
    }, 2000);
    return () => window.clearTimeout(timer);
  }, [rollouts, repository.customer_id, workspace]);

  if (openError) {
    return (
      <Card label="Edit" title="This File Cannot Be Edited Here" actions={<Button variant="ghost" onClick={onClose}>Close</Button>}>
        <Notice tone="warning">{openError}</Notice>
      </Card>
    );
  }
  if (!opened) {
    return (
      <Card label="Edit">
        <Skeleton lines={6} />
      </Card>
    );
  }

  const changed = text !== opened.text;
  const commit = async (redeploy: boolean) => {
    setBusy(redeploy ? "redeploy" : "commit");
    setError(null);
    try {
      const input = commitInput(repository.id, path, text, opened.eol, opened.blob, message || defaultMessage(path));
      const done = await forgeAct<{ commitOid?: string }>("commit_repository_files", input);
      const revision = String(done.commitOid ?? "");
      setCommitted(revision);
      // The editor now stands on what it committed: a further save is checked against it.
      const fresh = await openFile(repository.id, path).catch(() => null);
      if (fresh) {
        setOpened(fresh);
        setText(fresh.text);
      }
      setMessage("");
      onCommitted();
      if (redeploy && workspace) {
        const started: Rollout[] = [];
        for (const app of owners) {
          const deployed = await act<Row>("deploy_app", redeployInput(app, repository.customer_id, workspace, revision), { workspace });
          started.push({ app, generation: Number(deployed.generation ?? 0), state: "rolling" });
        }
        setRollouts(started);
      }
    } catch (e) {
      if (isConflict(e)) {
        // Changed on main since it was opened: show what is there now, keep the person's text.
        setConflict(await openFile(repository.id, path).catch(() => null));
      }
      setError(e);
    } finally {
      setBusy(null);
    }
  };

  const takeLatest = () => {
    if (!conflict) return;
    setOpened(conflict);
    setText(conflict.text);
    setConflict(null);
    setError(null);
  };
  const keepMine = () => {
    // Commit the person's text over the version they have now seen.
    if (!conflict) return;
    setOpened(conflict);
    setConflict(null);
    setError(null);
  };

  return (
    <Card
      label="Edit"
      title={`Editing ${path.split("/").pop() || path}`}
      actions={
        <Button variant="ghost" onClick={onClose} aria-label="Close the editor">
          <X size={14} aria-hidden /> Close
        </Button>
      }
    >
      {conflict ? (
        <div className="mb-3" role="alert">
          <Notice tone="danger" title="This file changed on main since you opened it">
            Nothing was committed. Here is what changed between the version on main and your text. Start again from the version on main, or keep your text and commit it over the version you have now seen.
          </Notice>
          <DriveDiff lines={lineDiff(conflict.text, text)} />
          <div className="mt-2 flex flex-wrap gap-2">
            <Button onClick={takeLatest}>
              <RefreshCw size={14} aria-hidden /> Use the version on main
            </Button>
            <Button onClick={keepMine}>Keep my text</Button>
          </div>
        </div>
      ) : error ? (
        <div className="mb-3">
          <Notice tone="danger">{error instanceof Error ? error.message : String(error)}</Notice>
        </div>
      ) : null}
      <label className="block">
        <span className="sr-only">Contents of {path}</span>
        <textarea
          className="min-h-[420px] w-full font-mono text-[12.5px] leading-5"
          spellCheck={false}
          autoCapitalize="off"
          autoCorrect="off"
          wrap="off"
          value={text}
          onChange={(event) => setText(event.target.value)}
          aria-label={`Contents of ${path}`}
        />
      </label>
      <p className="muted my-2 text-[12.5px]">
        UTF-8{opened.eol === "\r\n" ? ", CRLF line endings kept" : ""}. Committed to <code>main</code> as you.
      </p>
      <label className="block">
        Commit message
        <input value={message} onChange={(event) => setMessage(event.target.value)} placeholder={defaultMessage(path)} />
      </label>
      <div className="mt-3 flex flex-wrap gap-2">
        <Button variant="primary" disabled={!changed || !!conflict || busy !== null} loading={busy === "commit"} onClick={() => void commit(false)}>
          <Save size={14} aria-hidden /> Commit
        </Button>
        {owners.length > 0 && (
          <Button disabled={!changed || !!conflict || busy !== null} loading={busy === "redeploy"} onClick={() => void commit(true)} title={`Commit, then redeploy ${owners.map((app) => app.name).join(", ")} at the new revision`}>
            <Rocket size={14} aria-hidden /> Commit and redeploy
          </Button>
        )}
      </div>
      {owners.length > 0 && (
        <p className="muted mb-0 mt-2 text-[12.5px]">
          This file is in the source of {owners.map((app, index) => (
            <span key={app.id}>
              {index > 0 ? ", " : ""}
              <Link to={workspacePath(workspace, `/ship/app?customer_id=${encodeURIComponent(repository.customer_id)}&app_id=${encodeURIComponent(app.id)}`)}>{app.name}</Link>
            </span>
          ))}
          .
        </p>
      )}
      {committed ? (
        <p className="mb-0 mt-3 text-[13px]" role="status">
          Committed <code>{committed.slice(0, 12)}</code> to main.
        </p>
      ) : null}
      {rollouts.length > 0 && (
        <ul className="mb-0 mt-2 list-none p-0" aria-label="Redeploys" aria-live="polite">
          {rollouts.map((rollout) => (
            <li key={rollout.app.id} className="flex flex-wrap items-center gap-2 text-[13px]">
              {rollout.state === "rolling" ? <Loader2 size={14} className="animate-spin" aria-hidden /> : null}
              <span className="font-semibold">{rollout.app.name}</span>
              <Status value={rollout.state === "rolling" ? "rolling out" : rollout.state === "ready" ? "ready" : "failed"} tone={rollout.state === "ready" ? "success" : rollout.state === "failed" ? "danger" : "info"} />
              {rollout.message && rollout.state === "failed" ? <span className="muted">{rollout.message}</span> : null}
            </li>
          ))}
        </ul>
      )}
    </Card>
  );
}
