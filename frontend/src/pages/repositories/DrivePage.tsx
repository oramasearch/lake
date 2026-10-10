/** /drive?repository_id=&ref=&path=&view=&sort=&direction= (web_ui_drive_folder): a Drive folder as a
 * file manager. The breadcrumb and the folder's figures; a toolbar to filter by name, sort (by
 * name, size or modified, either direction) and switch between list and grid, which write the same
 * query parameters; a drop zone that uploads what is dropped or chosen; and each item's actions
 * (history, download, rename, move, delete) in a menu. New folder opens a dialog. Every write is a
 * new change; earlier versions stay in history. */
import { type DragEvent, useRef, useState } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import {
  ArrowDownAZ,
  ArrowUpAZ,
  Download,
  FileText,
  Folder,
  FolderInput,
  FolderOpen,
  FolderPlus,
  GitCompareArrows,
  History,
  LayoutGrid,
  List,
  Pencil,
  Share2,
  Trash2,
  Upload as UploadIcon,
} from "lucide-react";
import { useToast } from "@/lib/toast";
import { Modal } from "@/lib/overlay";
import { Banner, Button, DataTable, Empty, Field, SearchInput, Stat, StatRow, Toolbar } from "@/lib/ui";
import { dateTime, relative } from "@/lib/format";
import {
  bytesToBase64,
  displaySize,
  DriveBreadcrumbs,
  driveHref,
  EntryIcon,
  forgeAct,
  ForgePage,
  itemsOf,
  Menu,
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
  const [filter, setFilter] = useState("");
  const [creating, setCreating] = useState(false);
  const browse = useForgeRead<Browse>("drive_browse", { repository_id: id, ref, path, sort, direction });
  useRepositoryTrail(browse.data?.repository, "Files");
  const upload = useUpload(id, ref, path);
  const wait = pending(browse);
  if (wait) return wait;
  const r = browse.data!.repository;
  const entries = itemsOf(browse.data);
  const display = (next: Partial<{ view: string; sort: string; direction: string }>) => {
    const d = { view, sort, direction, ...next };
    setParams(new URLSearchParams({ repository_id: id, ref, path, view: d.view, sort: d.sort, direction: d.direction }));
  };
  const needle = filter.trim().toLowerCase();
  const shown = needle ? entries.filter((entry) => pathName(entry.path).toLowerCase().includes(needle)) : entries;
  const folders = entries.filter((entry) => entry.kind === "tree");
  const files = entries.filter((entry) => entry.kind !== "tree");
  const size = files.reduce((sum, entry) => sum + entry.size, 0);

  const actions = (
    <>
      <Button to={driveHref("/drive/share", id, ref, path)}>
        <Share2 size={14} aria-hidden /> Share
      </Button>
      <Button onClick={() => setCreating(true)}>
        <FolderPlus size={14} aria-hidden /> New folder
      </Button>
      <Button variant="primary" loading={upload.busy} onClick={() => upload.choose()}>
        {!upload.busy && <UploadIcon size={14} aria-hidden />} Upload
      </Button>
      <Menu
        label="Folder actions"
        items={[
          { label: "History", icon: History, to: driveHref("/drive/history", id, ref, path) },
          { label: "Conflicts", icon: GitCompareArrows, to: `/conflicts?repository_id=${encodeURIComponent(id)}` },
        ]}
      />
    </>
  );

  return (
    <ForgePage
      above={<DriveBreadcrumbs repository={r} refName={ref} path={path} />}
      title={path ? pathName(path) : repositoryName(r)}
      repository={r}
      tab="files"
      sub={
        <>
          Folder <code>{path || "/"}</code> at <code>{ref}</code>. Moves and deletes create new history; prior versions remain available.
        </>
      }
      actions={actions}
    >
      <StatRow cols={3}>
        <Stat label="Folders" value={folders.length} icon={Folder} sub="in this folder" />
        <Stat label="Files" value={files.length} icon={FileText} sub="in this folder" />
        <Stat label="Size" value={displaySize(size)} icon={FolderOpen} sub="of these files" />
      </StatRow>
      {upload.error && (
        <Banner tone="warn" title="Upload refused">
          {upload.error}
        </Banner>
      )}
      <Toolbar>
        <SearchInput value={filter} onChange={setFilter} placeholder="Filter by name…" label="Filter by name" />
        <div className="ml-auto flex flex-wrap items-center gap-2">
          <label className="flex items-center gap-1.5 text-[12.5px] text-ink-2">
            <span className="label m-0">Sort</span>
            <select aria-label="Sort" value={sort} onChange={(e) => display({ sort: e.target.value })}>
              <option value="name">name</option>
              <option value="size">size</option>
              <option value="modified">modified</option>
            </select>
          </label>
          <Button
            variant="ghost"
            aria-label={direction === "asc" ? "Ascending; sort descending" : "Descending; sort ascending"}
            title={direction === "asc" ? "Ascending" : "Descending"}
            onClick={() => display({ direction: direction === "asc" ? "desc" : "asc" })}
          >
            {direction === "asc" ? <ArrowDownAZ size={15} aria-hidden /> : <ArrowUpAZ size={15} aria-hidden />}
          </Button>
          <div className="view-toggle" role="group" aria-label="View">
            <button type="button" aria-pressed={view === "list"} aria-label="List" title="List" onClick={() => display({ view: "list" })}>
              <List size={15} aria-hidden />
            </button>
            <button type="button" aria-pressed={view === "grid"} aria-label="Grid" title="Grid" onClick={() => display({ view: "grid" })}>
              <LayoutGrid size={15} aria-hidden />
            </button>
          </div>
        </div>
      </Toolbar>
      <DropZone upload={upload} />
      <div className="mt-3">
        {entries.length === 0 ? (
          <Empty
            icon={FolderOpen}
            title="This folder is empty"
            action={
              <>
                <Button onClick={() => setCreating(true)}>New folder</Button>
                <Button variant="primary" onClick={() => upload.choose()}>
                  Upload files
                </Button>
              </>
            }
          >
            Upload files or create a folder to start.
          </Empty>
        ) : shown.length === 0 ? (
          <Empty compact title="Nothing matches">
            No item here is named like “{filter}”.
          </Empty>
        ) : view === "grid" ? (
          <div className="grid grid-cols-2 gap-3 sm:grid-cols-3 lg:grid-cols-4 xl:grid-cols-5">
            {shown.map((entry) => (
              <article key={entry.path} className="relative flex min-w-0 flex-col rounded-[6px] border border-hairline bg-surface p-3 transition-colors hover:border-rule">
                <div className="mb-3 flex items-start justify-between gap-2">
                  <span className="flex h-9 w-9 items-center justify-center rounded-[6px] bg-raised">
                    <EntryIcon kind={entry.kind} size={18} />
                  </span>
                  <EntryActions id={id} refName={ref} entry={entry} />
                </div>
                <div className="min-w-0 truncate text-[13.5px] font-semibold">
                  <EntryLink id={id} refName={ref} entry={entry} />
                </div>
                <div className="mt-0.5 truncate text-[12px] text-ink-3">
                  {entry.kind === "tree" ? "folder" : displaySize(entry.size)} · <span title={entry.modified_at ? dateTime(entry.modified_at) : undefined}>{entry.modified_at ? relative(entry.modified_at) : "—"}</span>
                </div>
              </article>
            ))}
          </div>
        ) : (
          <DataTable<TreeEntry>
            what="folder"
            caption="Folder contents"
            rows={shown}
            rowKey={(entry) => entry.path}
            columns={[
              {
                key: "name",
                label: "Name",
                render: (entry) => (
                  <span className="flex min-w-0 items-center gap-2.5">
                    <EntryIcon kind={entry.kind} />
                    <EntryLink id={id} refName={ref} entry={entry} />
                  </span>
                ),
              },
              { key: "kind", label: "Kind", render: (entry) => <span className="text-[12.5px] text-ink-2">{entry.kind === "tree" ? "folder" : entry.kind === "blob" ? "file" : entry.kind}</span> },
              { key: "size", label: "Size", numeric: true, render: (entry) => <span className="font-mono text-[12px]">{entry.kind === "tree" ? "—" : displaySize(entry.size)}</span> },
              {
                key: "modified",
                label: "Modified",
                render: (entry) => (
                  <span className="font-mono text-[12px] text-ink-2" title={entry.modified_at ? dateTime(entry.modified_at) : undefined}>
                    {entry.modified_at ? relative(entry.modified_at) : "—"}
                  </span>
                ),
              },
              { key: "actions", label: <span className="sr-only">Actions</span>, className: "w-10 text-right", render: (entry) => <EntryActions id={id} refName={ref} entry={entry} /> },
            ]}
          />
        )}
      </div>
      {creating && <NewFolder id={id} refName={ref} path={path} onClose={() => setCreating(false)} />}
    </ForgePage>
  );
}

function EntryLink({ id, refName, entry }: { id: string; refName: string; entry: TreeEntry }) {
  return <Link to={driveHref(entry.kind === "tree" ? "/drive" : "/drive/preview", id, refName, entry.path)}>{pathName(entry.path)}</Link>;
}

/** "New folder" (serve_drive_folder_create): a folder in the current folder. */
function NewFolder({ id, refName, path, onClose }: { id: string; refName: string; path: string; onClose: () => void }) {
  const toast = useToast();
  const [name, setName] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  return (
    <Modal
      title="New Folder"
      subtitle={`In /${path}`}
      onClose={onClose}
      width={440}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button type="submit" form="drive-new-folder" variant="primary" loading={busy}>
            Create folder
          </Button>
        </>
      }
    >
      <form
        id="drive-new-folder"
        className="stack"
        onSubmit={async (e) => {
          e.preventDefault();
          setBusy(true);
          setError(null);
          try {
            await forgeAct("create_drive_folder", { repository_id: id, ref: refName, path, name });
            toast("Folder created");
            onClose();
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
        <Field label="Name" required>
          <input value={name} onChange={(e) => setName(e.target.value)} autoFocus />
        </Field>
      </form>
    </Modal>
  );
}

type Uploader = ReturnType<typeof useUpload>;

/** "Upload" (serve_drive_upload): every chosen or dropped file in one change, sent as base64 JSON
 * (`upload_drive_files`) instead of the old multipart form. Choosing or dropping files uploads them
 * at once, as the drop script submitted the form. */
function useUpload(id: string, refName: string, path: string) {
  const toast = useToast();
  const input = useRef<HTMLInputElement>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const send = async (list: FileList | null) => {
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
      toast(files.length === 1 ? "File uploaded" : `${files.length} files uploaded`);
    } catch (err) {
      setError((err as Error).message);
    } finally {
      if (input.current) input.current.value = "";
      setBusy(false);
    }
  };
  return { input, busy, error, send, choose: () => input.current?.click() };
}

/** The drop zone: drop files anywhere on it, or press it to choose them. */
function DropZone({ upload }: { upload: Uploader }) {
  const [active, setActive] = useState(false);
  const over = (event: DragEvent) => {
    event.preventDefault();
    setActive(true);
  };
  const leave = (event: DragEvent) => {
    event.preventDefault();
    setActive(false);
  };
  return (
    <form id="drive-upload" onSubmit={(e) => e.preventDefault()}>
      <label
        id="drive-drop-zone"
        className="drive-drop-zone"
        data-active={active ? "true" : undefined}
        onDragEnter={over}
        onDragOver={over}
        onDragLeave={leave}
        onDrop={(event) => {
          leave(event);
          if (event.dataTransfer.files.length) void upload.send(event.dataTransfer.files);
        }}
      >
        <UploadIcon size={16} aria-hidden />
        <span>{upload.busy ? "Uploading…" : "Choose files or drop them here"}</span>
        <input ref={upload.input} id="drive-files" type="file" name="files" multiple className="sr-only" onChange={(e) => void upload.send(e.target.files)} />
      </label>
    </form>
  );
}

/** `drive_entry_actions`: History and Download, then rename, move and delete, in the item's menu. */
function EntryActions({ id, refName, entry }: { id: string; refName: string; entry: TreeEntry }) {
  const toast = useToast();
  const navigate = useNavigate();
  const [dialog, setDialog] = useState<"rename" | "move" | "delete" | null>(null);
  const [name, setName] = useState(pathName(entry.path));
  const [folder, setFolder] = useState("");
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const close = () => {
    setDialog(null);
    setError(null);
  };
  const move = async (kind: "rename" | "move", destination: string, after: string) => {
    setBusy(true);
    setError(null);
    try {
      await forgeAct("move_drive_path", { repository_id: id, ref: refName, path: entry.path, destination });
      toast(kind === "rename" ? "Renamed" : "Moved");
      close();
      navigate(driveHref("/drive", id, refName, after));
    } catch (err) {
      setError((err as Error).message);
    } finally {
      setBusy(false);
    }
  };
  const remove = async () => {
    setBusy(true);
    setError(null);
    try {
      await forgeAct("delete_drive_path", { repository_id: id, ref: refName, path: entry.path });
      toast("Deleted");
      close();
    } catch (err) {
      setError((err as Error).message);
    } finally {
      setBusy(false);
    }
  };
  const label = pathName(entry.path);
  const errorBlock = error && (
    <p className="error" role="alert">
      {error}
    </p>
  );
  return (
    <>
      <Menu
        label={`Actions for ${label}`}
        items={[
          { label: "History", icon: History, to: driveHref("/drive/history", id, refName, entry.path) },
          ...(entry.kind === "blob" ? [{ label: "Download", icon: Download, href: driveHref("/drive/download", id, refName, entry.path) }] : []),
          { label: "Rename", icon: Pencil, onSelect: () => setDialog("rename") },
          { label: "Move", icon: FolderInput, onSelect: () => setDialog("move") },
          { label: "Delete", icon: Trash2, danger: true, onSelect: () => setDialog("delete") },
        ]}
      />
      {dialog === "rename" && (
        <Modal
          title={`Rename ${label}`}
          onClose={close}
          width={440}
          footer={
            <>
              <Button onClick={close}>Cancel</Button>
              <Button type="submit" form={`rename-${entry.path}`} variant="primary" loading={busy}>
                Rename
              </Button>
            </>
          }
        >
          <form
            id={`rename-${entry.path}`}
            className="stack"
            onSubmit={(e) => {
              e.preventDefault();
              // serve_drive_rename: the new name in the same folder.
              const parent = parentPath(entry.path);
              const destination = parent ? `${parent}/${name}` : name;
              void move("rename", destination, parentPath(destination));
            }}
          >
            {errorBlock}
            <Field label="New name" required>
              <input value={name} onChange={(e) => setName(e.target.value)} autoFocus />
            </Field>
          </form>
        </Modal>
      )}
      {dialog === "move" && (
        <Modal
          title={`Move ${label}`}
          onClose={close}
          width={440}
          footer={
            <>
              <Button onClick={close}>Cancel</Button>
              <Button type="submit" form={`move-${entry.path}`} variant="primary" loading={busy}>
                Move
              </Button>
            </>
          }
        >
          <form
            id={`move-${entry.path}`}
            className="stack"
            onSubmit={(e) => {
              e.preventDefault();
              // serve_drive_move: the same name in the destination folder (empty is the root).
              const target = folder.replace(/^\/+|\/+$/g, "");
              const destination = target ? `${target}/${pathName(entry.path)}` : pathName(entry.path);
              void move("move", destination, target);
            }}
          >
            {errorBlock}
            <Field label="Destination folder" help="Leave empty to move it to the top of the repository.">
              <input value={folder} onChange={(e) => setFolder(e.target.value)} placeholder="team/archive" autoFocus />
            </Field>
          </form>
        </Modal>
      )}
      {dialog === "delete" && (
        <Modal
          title={`Delete ${label}?`}
          onClose={close}
          width={460}
          footer={
            <>
              <Button onClick={close}>Cancel</Button>
              <Button variant="danger" loading={busy} onClick={() => void remove()}>
                Delete
              </Button>
            </>
          }
        >
          <div className="muted">This writes a new change without it. Earlier versions stay in its history and can be restored.</div>
          {error && (
            <div className="error mt-3" role="alert">
              {error}
            </div>
          )}
        </Modal>
      )}
    </>
  );
}
