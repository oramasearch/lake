/** /drive?repository_id=&ref=&path=&view=&sort=&direction= (web_ui_drive_folder): a Drive folder in
 * a list or a grid, with new folder, upload (choose or drop files), and per entry history, rename,
 * move and delete. Every write is a new change; earlier versions stay in history. */
import { type DragEvent, useRef, useState } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { useToast } from "@/lib/toast";
import { ConfirmButton } from "@/lib/overlay";
import { Button } from "@/lib/ui";
import { dateTime } from "@/lib/format";
import {
  bytesToBase64,
  displaySize,
  DriveBreadcrumbs,
  driveHref,
  ForgeBadge,
  forgeAct,
  ForgePage,
  itemsOf,
  parentPath,
  pathName,
  pending,
  repositoryName,
  ToRepositories,
  useForgeRead,
  useRepositoryTrail,
  type Repository,
  type TreeEntry,
} from "./forge";

type Browse = { repository: Repository; path: string; items: TreeEntry[] };

/** The REST body limit (64 MiB, base64 and JSON included): larger uploads are refused before
 * they are sent. */
const MAX_BODY = 64 * 1024 * 1024;

export default function DrivePage() {
  const [params] = useSearchParams();
  const id = params.get("repository_id") ?? "";
  if (!id) return <ToRepositories />;
  return <DriveFolder id={id} />;
}

function DriveFolder({ id }: { id: string }) {
  const [params, setParams] = useSearchParams();
  const ref = params.get("ref") || "main";
  const path = params.get("path") ?? "";
  const view = params.get("view") === "grid" ? "grid" : "list";
  const sort = ["name", "size", "modified"].includes(params.get("sort") ?? "") ? (params.get("sort") as string) : "name";
  const direction = params.get("direction") === "desc" ? "desc" : "asc";
  const [display, setDisplay] = useState({ view, sort, direction });
  const browse = useForgeRead<Browse>("drive_browse", { repository_id: id, ref, path, sort, direction });
  useRepositoryTrail(browse.data?.repository, "Files");
  const wait = pending(browse);
  if (wait) return wait;
  const r = browse.data!.repository;
  const entries = itemsOf(browse.data);
  return (
    <ForgePage
      above={<DriveBreadcrumbs repository={r} refName={ref} path={path} />}
      eyebrow="drive"
      title={path ? pathName(path) : repositoryName(r)}
      sub={
        <>
          Folder <code>{path || "/"}</code> at <code>{ref}</code>. Moves and deletes create new history; prior versions remain available.
        </>
      }
    >
      <div className="actions">
        <Link to={driveHref("/drive/history", id, ref, path)}>History</Link>
        <Link to={driveHref("/drive/share", id, ref, path)}>Share</Link>
        <Link to={`/conflicts?repository_id=${encodeURIComponent(id)}`}>Conflicts</Link>
      </div>
      <form
        className="filters"
        aria-label="Folder display"
        onSubmit={(e) => {
          e.preventDefault();
          setParams(new URLSearchParams({ repository_id: id, ref, path, view: display.view, sort: display.sort, direction: display.direction }));
        }}
      >
        <label>
          View
          <select value={display.view} onChange={(e) => setDisplay({ ...display, view: e.target.value })}>
            <option value="list">List</option>
            <option value="grid">Grid</option>
          </select>
        </label>
        <label>
          Sort
          <select value={display.sort} onChange={(e) => setDisplay({ ...display, sort: e.target.value })}>
            <option value="name">name</option>
            <option value="size">size</option>
            <option value="modified">modified</option>
          </select>
        </label>
        <label>
          Direction
          <select value={display.direction} onChange={(e) => setDisplay({ ...display, direction: e.target.value })}>
            <option value="asc">Ascending</option>
            <option value="desc">Descending</option>
          </select>
        </label>
        <button type="submit">Apply</button>
      </form>
      <div className="drive-create grid">
        <NewFolder id={id} refName={ref} path={path} />
        <Upload id={id} refName={ref} path={path} />
      </div>
      {view === "grid" ? (
        <div className="drive-grid">
          {entries.map((entry) => (
            <article key={entry.path} className="card drive-card">
              <p>
                <ForgeBadge value={entry.kind} />
              </p>
              <h2>
                <EntryLink id={id} refName={ref} entry={entry} />
              </h2>
              <p className="muted">
                {entry.kind === "tree" ? "—" : displaySize(entry.size)} · {entry.modified_at ? dateTime(entry.modified_at) : "—"}
              </p>
              <EntryActions id={id} refName={ref} entry={entry} />
            </article>
          ))}
          {entries.length === 0 && <div className="panel empty">This folder is empty.</div>}
        </div>
      ) : (
        <div className="table-wrap">
          <table>
            <thead>
              <tr>
                <th>Name</th>
                <th>Size</th>
                <th>Modified</th>
                <th>Actions</th>
              </tr>
            </thead>
            <tbody>
              {entries.map((entry) => (
                <tr key={entry.path}>
                  <td>
                    <EntryLink id={id} refName={ref} entry={entry} /> <ForgeBadge value={entry.kind} />
                  </td>
                  <td>{entry.kind === "tree" ? "—" : displaySize(entry.size)}</td>
                  <td>{entry.modified_at ? dateTime(entry.modified_at) : "—"}</td>
                  <td>
                    <EntryActions id={id} refName={ref} entry={entry} />
                  </td>
                </tr>
              ))}
              {entries.length === 0 && (
                <tr>
                  <td colSpan={4} className="empty">
                    This folder is empty.
                  </td>
                </tr>
              )}
            </tbody>
          </table>
        </div>
      )}
    </ForgePage>
  );
}

function EntryLink({ id, refName, entry }: { id: string; refName: string; entry: TreeEntry }) {
  return <Link to={driveHref(entry.kind === "tree" ? "/drive" : "/drive/preview", id, refName, entry.path)}>{pathName(entry.path)}</Link>;
}

/** "New Folder" (serve_drive_folder_create): a folder in the current folder. */
function NewFolder({ id, refName, path }: { id: string; refName: string; path: string }) {
  const toast = useToast();
  const [name, setName] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  return (
    <section className="panel">
      <h2>New Folder</h2>
      <form
        className="stack"
        onSubmit={async (e) => {
          e.preventDefault();
          setBusy(true);
          setError(null);
          try {
            await forgeAct("create_drive_folder", { repository_id: id, ref: refName, path, name });
            setName("");
            toast("Folder created");
          } catch (err) {
            setError((err as Error).message);
          } finally {
            setBusy(false);
          }
        }}
      >
        {error && (
          <p className="error" role="alert">
            {error}
          </p>
        )}
        <label>
          Name
          <input value={name} onChange={(e) => setName(e.target.value)} required />
        </label>
        <div>
          <Button type="submit" loading={busy}>
            Create folder
          </Button>
        </div>
      </form>
    </section>
  );
}

/** "Upload" (serve_drive_upload): every chosen or dropped file in one change, sent as base64 JSON
 * (`upload_drive_files`) instead of the old multipart form. Dropping files uploads them at once,
 * as the drop script submitted the form. */
function Upload({ id, refName, path }: { id: string; refName: string; path: string }) {
  const toast = useToast();
  const input = useRef<HTMLInputElement>(null);
  const [active, setActive] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const upload = async (list: FileList | null) => {
    if (!list || list.length === 0) {
      setError("at least one uploaded file is required");
      return;
    }
    setBusy(true);
    setError(null);
    try {
      const files = await Promise.all([...list].map(async (file) => ({ name: file.name, content: bytesToBase64(new Uint8Array(await file.arrayBuffer())) })));
      const size = files.reduce((sum, file) => sum + file.content.length + file.name.length + 32, 256);
      if (size > MAX_BODY) throw new Error(`These files are too large to upload at once: ${displaySize(size)} encoded, of at most ${displaySize(MAX_BODY)}.`);
      await forgeAct("upload_drive_files", { repository_id: id, ref: refName, path, files });
      if (input.current) input.current.value = "";
      toast(files.length === 1 ? "File uploaded" : `${files.length} files uploaded`);
    } catch (err) {
      setError((err as Error).message);
    } finally {
      setBusy(false);
    }
  };
  const over = (event: DragEvent) => {
    event.preventDefault();
    setActive(true);
  };
  const leave = (event: DragEvent) => {
    event.preventDefault();
    setActive(false);
  };
  return (
    <section className="panel">
      <h2>Upload</h2>
      <form
        id="drive-upload"
        className="stack"
        onSubmit={(e) => {
          e.preventDefault();
          void upload(input.current?.files ?? null);
        }}
      >
        {error && (
          <p className="error" role="alert">
            {error}
          </p>
        )}
        <label
          id="drive-drop-zone"
          className="drive-drop-zone"
          data-active={active ? "true" : undefined}
          onDragEnter={over}
          onDragOver={over}
          onDragLeave={leave}
          onDrop={(event) => {
            leave(event);
            if (event.dataTransfer.files.length) void upload(event.dataTransfer.files);
          }}
        >
          Choose files or drop them here
          <input ref={input} id="drive-files" type="file" name="files" multiple required />
        </label>
        <div>
          <Button type="submit" loading={busy}>
            Upload files
          </Button>
        </div>
      </form>
    </section>
  );
}

/** `drive_entry_actions`: History, then Manage: rename, move and delete. */
function EntryActions({ id, refName, entry }: { id: string; refName: string; entry: TreeEntry }) {
  const toast = useToast();
  const navigate = useNavigate();
  const [name, setName] = useState(pathName(entry.path));
  const [folder, setFolder] = useState("");
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState<string | null>(null);
  const move = async (kind: "rename" | "move", destination: string, after: string) => {
    setBusy(kind);
    setError(null);
    try {
      await forgeAct("move_drive_path", { repository_id: id, ref: refName, path: entry.path, destination });
      toast(kind === "rename" ? "Renamed" : "Moved");
      navigate(driveHref("/drive", id, refName, after));
    } catch (err) {
      setError((err as Error).message);
    } finally {
      setBusy(null);
    }
  };
  return (
    <div className="drive-item-actions">
      <Link to={driveHref("/drive/history", id, refName, entry.path)}>History</Link>
      <details>
        <summary>Manage</summary>
        <div className="drive-manage">
          {error && (
            <p className="error" role="alert">
              {error}
            </p>
          )}
          <form
            className="stack"
            onSubmit={(e) => {
              e.preventDefault();
              // serve_drive_rename: the new name in the same folder.
              const parent = parentPath(entry.path);
              const destination = parent ? `${parent}/${name}` : name;
              void move("rename", destination, parentPath(destination));
            }}
          >
            <label>
              New name
              <input value={name} onChange={(e) => setName(e.target.value)} required />
            </label>
            <div>
              <Button type="submit" loading={busy === "rename"}>
                Rename
              </Button>
            </div>
          </form>
          <form
            className="stack"
            onSubmit={(e) => {
              e.preventDefault();
              // serve_drive_move: the same name in the destination folder (empty is the root).
              const target = folder.replace(/^\/+|\/+$/g, "");
              const destination = target ? `${target}/${pathName(entry.path)}` : pathName(entry.path);
              void move("move", destination, target);
            }}
          >
            <label>
              Destination folder
              <input value={folder} onChange={(e) => setFolder(e.target.value)} placeholder="team/archive" />
            </label>
            <div>
              <Button type="submit" loading={busy === "move"}>
                Move
              </Button>
            </div>
          </form>
          <div>
            <ConfirmButton
              variant="danger"
              title={`Delete ${pathName(entry.path)}?`}
              confirmLabel="Delete"
              confirm="This writes a new change without it. Earlier versions stay in its history and can be restored."
              onConfirm={async () => {
                await forgeAct("delete_drive_path", { repository_id: id, ref: refName, path: entry.path });
                toast("Deleted");
              }}
            >
              Delete
            </ConfirmButton>
          </div>
        </div>
      </details>
    </div>
  );
}
