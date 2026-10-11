/** One drive (`/w/<id>/drives/drive?id=<drive>&folder=<key>[&focus=<item>]`, lake#561,
 * docs/ui/drives.md): a file manager over the drive's `DriveStorage`, whatever keeps its files.
 * The folder's path, a toolbar (search, sort), the folder's items with their actions, and:
 *
 * - Search: what is typed filters this folder's items at once, then `search_drives` finds the
 *   matching names everywhere else in the drive, listed under the folder with their paths.
 * - Upload files or a whole folder (chosen, or dropped with its subfolders) into the folder, with
 *   progress; a large workspace file goes in chunks.
 * - Select items (shift-click selects a range) and move or delete them together; drag items onto
 *   a folder row or a crumb of the path to move them there.
 * - New folder; New shortcut in a workspace-files or virtual drive, each shortcut marked when what
 *   it points at is gone.
 * - Rename or delete the drive (its name typed).
 *
 * A repository drive's folders also open in the repository's own Drive pages for history and
 * sharing. */
import { type DragEvent, type MouseEvent, useEffect, useMemo, useRef, useState } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { useQuery } from "@tanstack/react-query";
import {
  AlertTriangle,
  ArrowDownAZ,
  ArrowUpAZ,
  ChevronRight,
  Database,
  Download,
  ExternalLink,
  Eye,
  FileText,
  Folder,
  FolderInput,
  FolderOpen,
  FolderPlus,
  FolderUp,
  HardDrive,
  History,
  Link2,
  Pencil,
  Settings,
  Share2,
  Trash2,
  Upload as UploadIcon,
  X,
} from "lucide-react";
import { act, queryClient, read, refreshAfterWrite, useRead } from "@/lib/api";
import { dateTime, relative } from "@/lib/format";
import { Modal } from "@/lib/overlay";
import { useToast } from "@/lib/toast";
import { useTrail } from "@/lib/trail";
import { Banner, Button, Chip, Empty, ErrorBlock, Field, FormError, Meter, Page, SearchInput, Skeleton, Stat, StatRow, Toolbar, cx, fieldError } from "@/lib/ui";
import { WorkspaceGate } from "@/components/WorkspaceGate";
import { Menu, displaySize, driveHref as repositoryDriveHref } from "@/pages/repositories/forge";
import { qs, text, useWorkspace, useWsPath } from "@/pages/train/workspace";
import { datasetFormat, datasetName, rangeSelect, sortEntries, validName, type Direction, type SortKey, type UploadItem } from "./paths";
import { storageFor, type Drive, type DriveStorage, type Item, type Listing, type Progress, type Upload } from "./storage";
import { STORAGE, storageLabel, targetHref, targetLabel } from "./targets";
import { ShortcutDialog } from "./ShortcutDialog";
import { matchLink, matchWhere, parentKey, folderKey, type DriveMatch, type SearchDrivesResult } from "./search";
import { MatchAnchor, matchIcon } from "./MatchLink";
import { Highlight } from "@/components/Highlight";
import { FolderPicker } from "./FolderPicker";

/** The type an internal drag carries, so a drop of files from the desktop is never a move. */
const DRAG_TYPE = "application/x-lake-drive-items";
/** A button that reads as a link (a folder's name, a crumb of the path). */
const LINK = "cursor-pointer border-0 bg-transparent p-0 text-left text-action hover:underline";

export default function DrivePage() {
  const [params] = useSearchParams();
  const id = params.get("id") ?? "";
  const wsPath = useWsPath();
  if (!id) {
    return (
      <Page eyebrow="Data" title="Drive">
        <Empty icon={HardDrive} title="No drive chosen" action={<Button to={wsPath("/drives")}>All drives</Button>}>
          Open a drive from the Drives page.
        </Empty>
      </Page>
    );
  }
  return <DriveLoader id={id} />;
}

function DriveLoader({ id }: { id: string }) {
  const { customerId, workspaceId } = useWorkspace();
  const ready = !!customerId && !!workspaceId;
  const drive = useRead<Drive>(ready ? "get_drive" : null, { customer_id: customerId, workspace_id: workspaceId, id });
  return (
    <WorkspaceGate returnTo={`/drives/drive?${qs({ id })}`}>
      {drive.isLoading || !ready ? (
        <Page eyebrow="Drive" title="Drive">
          <Skeleton lines={5} />
        </Page>
      ) : drive.isError ? (
        <Page eyebrow="Drive" title="Drive">
          <ErrorBlock error={drive.error} retry={() => void drive.refetch()} what="the drive" />
        </Page>
      ) : (
        <DriveBrowser drive={drive.data!} customerId={customerId} workspaceId={workspaceId} />
      )}
    </WorkspaceGate>
  );
}

type Dialog =
  | { kind: "folder" }
  | { kind: "shortcut" }
  | { kind: "rename"; item: Item }
  | { kind: "move"; items: Item[] }
  | { kind: "delete"; items: Item[] }
  | { kind: "drive" }
  | { kind: "dataset"; item: Item };

function DriveBrowser({ drive, customerId, workspaceId }: { drive: Drive; customerId: string; workspaceId: string }) {
  const [params, setParams] = useSearchParams();
  const navigate = useNavigate();
  const toast = useToast();
  const wsPath = useWsPath();
  const folder = params.get("folder") ?? "";
  const focus = params.get("focus") ?? "";
  const storage = useMemo(() => storageFor(drive, { customerId, workspaceId }), [drive, customerId, workspaceId]);
  const [filter, setFilter] = useState("");
  const [sort, setSort] = useState<SortKey>("name");
  const [direction, setDirection] = useState<Direction>("asc");
  const [selected, setSelected] = useState<Set<string>>(new Set());
  const [anchor, setAnchor] = useState<string | null>(null);
  const [dialog, setDialog] = useState<Dialog | null>(null);
  const [dropTarget, setDropTarget] = useState<string | null>(null);
  const upload = useUploader(storage, folder);

  const listing = useQuery<Listing>({
    queryKey: ["drive-listing", drive.id, drive.storage, workspaceId, folder],
    queryFn: ({ signal }) => storage.list(folder, signal),
  });

  // A shortcut to an item of this drive opens the folder that holds it.
  useEffect(() => {
    if (!focus || drive.storage === "repository") return;
    let cancelled = false;
    void read<{ parent_id: string | null }>("get_drive_entry", { customer_id: customerId, workspace_id: workspaceId, drive_id: drive.id, id: focus })
      .then((entry) => {
        if (cancelled) return;
        setParams(new URLSearchParams({ id: drive.id, folder: entry.parent_id ?? "" }), { replace: true });
        setSelected(new Set([focus]));
      })
      .catch(() => {
        if (!cancelled) setParams(new URLSearchParams({ id: drive.id }), { replace: true });
      });
    return () => {
      cancelled = true;
    };
  }, [focus, drive.id, drive.storage, customerId, workspaceId, setParams]);

  const open = (key: string) => {
    setSelected(new Set());
    setAnchor(null);
    setParams(new URLSearchParams({ id: drive.id, ...(key ? { folder: key } : {}) }));
  };

  const crumbs = listing.data?.crumbs ?? [];
  const here = crumbs.length ? crumbs[crumbs.length - 1].name : drive.name;
  useTrail([{ label: "Drives", to: wsPath("/drives") }, { label: drive.name, to: wsPath(`/drives/drive?${qs({ id: drive.id })}`) }, ...crumbs.map((c) => ({ label: c.name, to: wsPath(`/drives/drive?${qs({ id: drive.id, folder: c.key })}`) }))]);

  const items = useMemo(() => listing.data?.items ?? [], [listing.data]);
  const needle = filter.trim().toLowerCase();
  const shown = useMemo(
    () =>
      sortEntries(
        items.filter((item) => !needle || item.name.toLowerCase().includes(needle)).map((item) => ({ ...item, path: item.name })),
        sort,
        direction,
      ),
    [items, needle, sort, direction],
  );
  const order = shown.map((item) => item.key);
  const chosen = items.filter((item) => selected.has(item.key));
  const folders = items.filter((item) => item.kind === "folder").length;
  const files = items.filter((item) => item.kind === "file");
  const shortcuts = items.filter((item) => item.kind === "shortcut");
  const broken = shortcuts.filter((item) => item.status === "broken").length;
  const stats = (1 + (drive.storage !== "virtual" ? 2 : 0) + (storage.can.shortcuts ? 1 : 0)) as 2 | 3 | 4;

  const select = (item: Item, event: MouseEvent) => {
    const next = new Set(event.metaKey || event.ctrlKey || event.shiftKey ? selected : []);
    if (event.shiftKey) {
      for (const key of rangeSelect(order, anchor, item.key)) next.add(key);
    } else if (next.has(item.key) && (event.metaKey || event.ctrlKey)) {
      next.delete(item.key);
    } else {
      next.add(item.key);
    }
    if (!event.shiftKey) setAnchor(item.key);
    setSelected(next);
  };
  const toggle = (item: Item, event: MouseEvent) => {
    event.stopPropagation();
    if (event.shiftKey) return select(item, event);
    const next = new Set(selected);
    if (next.has(item.key)) next.delete(item.key);
    else next.add(item.key);
    setAnchor(item.key);
    setSelected(next);
  };

  const moveTo = async (dragged: Item[], target: string) => {
    try {
      const report = await storage.move(dragged, target);
      refreshAfterWrite();
      setSelected(new Set());
      const skipped = report.skipped.length ? ` ${report.skipped.map((s) => `${s.name}: ${s.reason}`).join("; ")}.` : "";
      toast(report.moved ? `Moved ${report.moved === 1 ? "1 item" : `${report.moved} items`}.${skipped}` : `Nothing moved.${skipped}`);
    } catch (error) {
      toast(`Not moved: ${(error as Error).message}`);
    }
  };

  // Internal drags carry the keys of the items dragged; anything else dropped is files.
  const dragStart = (item: Item, event: DragEvent) => {
    const keys = selected.has(item.key) ? [...selected] : [item.key];
    if (!selected.has(item.key)) setSelected(new Set([item.key]));
    event.dataTransfer.setData(DRAG_TYPE, JSON.stringify(keys));
    event.dataTransfer.effectAllowed = "move";
  };
  const dropOn = (target: string) => ({
    onDragOver: (event: DragEvent) => {
      if (!event.dataTransfer.types.includes(DRAG_TYPE) && !(storage.can.upload && event.dataTransfer.types.includes("Files"))) return;
      event.preventDefault();
      event.stopPropagation();
      setDropTarget(target);
    },
    onDragLeave: () => setDropTarget((current) => (current === target ? null : current)),
    onDrop: (event: DragEvent) => {
      event.preventDefault();
      event.stopPropagation();
      setDropTarget(null);
      const raw = event.dataTransfer.getData(DRAG_TYPE);
      if (raw) {
        const keys = new Set<string>(JSON.parse(raw) as string[]);
        void moveTo(
          items.filter((item) => keys.has(item.key) && item.key !== target),
          target,
        );
      } else if (storage.can.upload) {
        void droppedFiles(event).then((dropped) => upload.send(dropped, target));
      }
    },
  });

  const actions = (
    <>
      {storage.can.shortcuts && (
        <Button onClick={() => setDialog({ kind: "shortcut" })}>
          <Link2 size={14} aria-hidden /> New shortcut
        </Button>
      )}
      <Button onClick={() => setDialog({ kind: "folder" })}>
        <FolderPlus size={14} aria-hidden /> New folder
      </Button>
      {storage.can.upload && (
        <Button variant="primary" loading={upload.busy} onClick={() => upload.chooseFiles()}>
          {!upload.busy && <UploadIcon size={14} aria-hidden />} Upload
        </Button>
      )}
      <Menu
        label="Drive actions"
        items={[
          ...(storage.can.upload ? [{ label: "Upload a folder", icon: FolderUp, onSelect: () => upload.chooseFolder() }] : []),
          ...(drive.storage === "repository" && drive.repository_id
            ? [
                { label: "History", icon: History, to: repositoryDriveHref("/drive/history", drive.repository_id, "main", folder) },
                { label: "Share this folder", icon: Share2, to: repositoryDriveHref("/drive/share", drive.repository_id, "main", folder) },
                { label: "Open the repository", icon: ExternalLink, to: `/repository?${qs({ id: drive.repository_id })}` },
              ]
            : []),
          { label: "Drive settings", icon: Settings, onSelect: () => setDialog({ kind: "drive" }) },
        ]}
      />
    </>
  );

  return (
    <Page
      eyebrow={`Drive · ${storageLabel(drive.storage)}`}
      title={here}
      sub={crumbs.length ? <PathBar drive={drive} crumbs={crumbs} open={open} dropOn={dropOn} dropTarget={dropTarget} /> : STORAGE.find((s) => s.id === drive.storage)?.detail}
      actions={actions}
    >
      <input ref={upload.filesInput} type="file" multiple className="sr-only" aria-label="Files to upload" onChange={(e) => void upload.fromInput(e.target)} />
      <input
        ref={upload.folderInput}
        type="file"
        multiple
        className="sr-only"
        aria-label="Folder to upload"
        // A folder chosen whole: every file arrives with its path inside the folder.
        {...({ webkitdirectory: "" } as Record<string, string>)}
        onChange={(e) => void upload.fromInput(e.target)}
      />
      <StatRow cols={stats}>
        <Stat label="Folders" value={listing.isLoading ? "…" : folders} icon={Folder} sub="in this folder" />
        {drive.storage !== "virtual" && <Stat label="Files" value={listing.isLoading ? "…" : files.length} icon={FileText} sub={displaySize(files.reduce((sum, f) => sum + f.size, 0))} />}
        {storage.can.shortcuts && <Stat label="Shortcuts" value={listing.isLoading ? "…" : shortcuts.length} icon={Link2} sub={broken ? `${broken} broken` : "all reachable"} />}
        {drive.storage !== "virtual" && <Stat label="History" value={drive.storage === "repository" ? "Kept" : "None"} icon={History} sub={drive.storage === "repository" ? "every change can be restored" : "a deleted file is gone"} />}
      </StatRow>
      {upload.progress && (
        <Banner tone="info" title={`Uploading ${upload.progress.file}`}>
          <Meter value={upload.progress.total ? (upload.progress.sent / upload.progress.total) * 100 : 0} label={`${displaySize(upload.progress.sent)} of ${displaySize(upload.progress.total)}`} />
        </Banner>
      )}
      {upload.error && (
        <Banner tone="warn" title="Upload stopped" action={<Button variant="ghost" onClick={upload.clear}>Dismiss</Button>}>
          {upload.error}
        </Banner>
      )}
      {(listing.data?.uploads?.length ?? 0) > 0 && <UnfinishedUploads storage={storage} uploads={listing.data!.uploads!} />}
      {upload.refused.length > 0 && (
        <Banner tone="warn" title="Some files were not uploaded" action={<Button variant="ghost" onClick={upload.clear}>Dismiss</Button>}>
          {upload.refused.join(", ")}
        </Banner>
      )}
      <Toolbar>
        <SearchInput value={filter} onChange={setFilter} placeholder="Search this drive…" label="Search this drive" />
        <div className="ml-auto flex flex-wrap items-center gap-2">
          <label className="flex items-center gap-1.5 text-[12.5px] text-ink-2">
            <span className="label m-0">Sort</span>
            <select aria-label="Sort" value={sort} onChange={(e) => setSort(e.target.value as SortKey)}>
              <option value="name">name</option>
              <option value="size">size</option>
              <option value="modified">modified</option>
            </select>
          </label>
          <Button variant="ghost" aria-label={direction === "asc" ? "Ascending; sort descending" : "Descending; sort ascending"} onClick={() => setDirection(direction === "asc" ? "desc" : "asc")}>
            {direction === "asc" ? <ArrowDownAZ size={15} aria-hidden /> : <ArrowUpAZ size={15} aria-hidden />}
          </Button>
        </div>
      </Toolbar>
      {chosen.length > 0 && (
        <div className="mb-2 flex flex-wrap items-center gap-2 rounded-[6px] border border-action/40 bg-action-soft px-3 py-2 text-[13px]" role="region" aria-label="Selection">
          <strong>{chosen.length === 1 ? "1 item selected" : `${chosen.length} items selected`}</strong>
          <Button onClick={() => setDialog({ kind: "move", items: chosen })}>
            <FolderInput size={14} aria-hidden /> Move
          </Button>
          <Button variant="danger" onClick={() => setDialog({ kind: "delete", items: chosen })}>
            <Trash2 size={14} aria-hidden /> Delete
          </Button>
          <Button variant="ghost" onClick={() => setSelected(new Set())}>
            <X size={14} aria-hidden /> Clear
          </Button>
        </div>
      )}
      <div
        className={cx("rounded-[6px]", dropTarget === "__here" && "outline outline-2 outline-action")}
        onDragOver={(event) => {
          if (storage.can.upload && event.dataTransfer.types.includes("Files")) {
            event.preventDefault();
            setDropTarget("__here");
          }
        }}
        onDragLeave={() => setDropTarget((current) => (current === "__here" ? null : current))}
        onDrop={(event) => {
          event.preventDefault();
          setDropTarget(null);
          if (storage.can.upload && !event.dataTransfer.types.includes(DRAG_TYPE)) void droppedFiles(event).then((dropped) => upload.send(dropped, folder));
        }}
      >
        {listing.isLoading ? (
          <Skeleton lines={5} />
        ) : listing.isError ? (
          <ErrorBlock error={listing.error} retry={() => void listing.refetch()} what="this folder" />
        ) : items.length === 0 ? (
          <Empty
            icon={FolderOpen}
            title="This folder is empty"
            action={
              <>
                <Button onClick={() => setDialog({ kind: "folder" })}>New folder</Button>
                {storage.can.shortcuts && <Button onClick={() => setDialog({ kind: "shortcut" })}>New shortcut</Button>}
                {storage.can.upload && (
                  <Button variant="primary" onClick={() => upload.chooseFiles()}>
                    Upload files
                  </Button>
                )}
              </>
            }
          >
            {storage.can.upload ? "Drop files or folders here, or upload them." : "Add folders and shortcuts to what this workspace already has."}
          </Empty>
        ) : shown.length === 0 ? (
          <Empty compact title="Nothing in This Folder Matches">
            No item here is named like “{filter}”. Matches elsewhere in the drive are listed below.
          </Empty>
        ) : (
          <div className="table-wrap" role="region" aria-label="Folder contents" tabIndex={0}>
            <table>
              <thead>
                <tr>
                  <th scope="col" className="w-8">
                    <input
                      type="checkbox"
                      aria-label="Select every item"
                      checked={shown.length > 0 && shown.every((item) => selected.has(item.key))}
                      onChange={(e) => setSelected(e.target.checked ? new Set(order) : new Set())}
                    />
                  </th>
                  <th scope="col">Name</th>
                  <th scope="col">Kind</th>
                  <th scope="col" className="text-right">
                    Size
                  </th>
                  <th scope="col">Modified</th>
                  <th scope="col" className="w-10 text-right">
                    <span className="sr-only">Actions</span>
                  </th>
                </tr>
              </thead>
              <tbody>
                {shown.map((item) => (
                  <tr
                    key={item.key}
                    draggable
                    onDragStart={(event) => dragStart(item, event)}
                    onClick={(event) => select(item, event)}
                    aria-selected={selected.has(item.key)}
                    className={cx(selected.has(item.key) && "bg-action-soft", dropTarget === item.key && "outline outline-2 outline-action")}
                    {...(item.kind === "folder" ? dropOn(item.key) : {})}
                  >
                    <td>
                      <input type="checkbox" aria-label={`Select ${item.name}`} checked={selected.has(item.key)} onClick={(e) => toggle(item, e)} onChange={() => undefined} />
                    </td>
                    <td>
                      <ItemName item={item} storage={storage} open={open} wsPath={wsPath} />
                    </td>
                    <td className="text-[12.5px] text-ink-2">{item.kind === "shortcut" ? targetLabel(item.target?.kind) : item.kind}</td>
                    <td className="text-right font-mono text-[12px] tabular-nums">{item.kind === "file" ? displaySize(item.size) : "—"}</td>
                    <td className="font-mono text-[12px] text-ink-2" title={item.modifiedAt ? dateTime(item.modifiedAt) : undefined}>
                      {item.modifiedAt ? relative(item.modifiedAt) : "—"}
                    </td>
                    <td className="text-right" onClick={(e) => e.stopPropagation()}>
                      <Menu
                        label={`Actions for ${item.name}`}
                        items={[
                          ...(item.kind === "folder" ? [{ label: "Open", icon: FolderOpen, onSelect: () => open(item.key) }] : []),
                          ...(storage.viewHref(item) ? [{ label: "View", icon: Eye, href: storage.viewHref(item)! }] : []),
                          ...(storage.downloadHref(item) ? [{ label: "Download", icon: Download, href: storage.downloadHref(item)! }] : []),
                          ...(drive.storage === "repository" && drive.repository_id ? [{ label: "History", icon: History, to: repositoryDriveHref("/drive/history", drive.repository_id, "main", item.key) }] : []),
                          ...(item.kind === "file" && datasetFormat(item.name) ? [{ label: "Load as dataset", icon: Database, onSelect: () => setDialog({ kind: "dataset", item }) }] : []),
                          { label: "Rename", icon: Pencil, onSelect: () => setDialog({ kind: "rename", item }) },
                          { label: "Move", icon: FolderInput, onSelect: () => setDialog({ kind: "move", items: [item] }) },
                          { label: "Delete", icon: Trash2, danger: true, onSelect: () => setDialog({ kind: "delete", items: [item] }) },
                        ]}
                      />
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </div>
      {needle && <DriveMatches drive={drive} folder={folder} q={filter.trim()} scope={{ customerId, workspaceId }} open={open} wsPath={wsPath} />}
      {dialog?.kind === "folder" && <NewFolder storage={storage} folder={folder} onClose={() => setDialog(null)} />}
      {dialog?.kind === "shortcut" && <ShortcutDialog storage={storage} folder={folder} onClose={() => setDialog(null)} />}
      {dialog?.kind === "rename" && <Rename storage={storage} item={dialog.item} onClose={() => setDialog(null)} />}
      {dialog?.kind === "move" && (
        <FolderPicker
          storage={storage}
          start={folder}
          title={dialog.items.length === 1 ? `Move ${dialog.items[0].name}` : `Move ${dialog.items.length} Items`}
          action="Move here"
          exclude={new Set(dialog.items.filter((i) => i.kind === "folder").map((i) => i.key))}
          onClose={() => setDialog(null)}
          onPick={async (target) => {
            setDialog(null);
            await moveTo(dialog.items, target);
          }}
        />
      )}
      {dialog?.kind === "delete" && <Delete storage={storage} items={dialog.items} onClose={() => setDialog(null)} onDone={() => setSelected(new Set())} />}
      {dialog?.kind === "dataset" && <LoadDataset drive={drive} item={dialog.item} customerId={customerId} workspaceId={workspaceId} onClose={() => setDialog(null)} />}
      {dialog?.kind === "drive" && (
        <DriveSettings
          drive={drive}
          customerId={customerId}
          workspaceId={workspaceId}
          onClose={() => setDialog(null)}
          onDeleted={() => {
            toast(`Drive “${drive.name}” deleted`);
            navigate(wsPath("/drives"));
          }}
        />
      )}
    </Page>
  );
}

/** The value, once it has stopped changing for `ms`. */
function useSettled<T>(value: T, ms: number): T {
  const [settled, setSettled] = useState(value);
  useEffect(() => {
    const timer = setTimeout(() => setSettled(value), ms);
    return () => clearTimeout(timer);
  }, [value, ms]);
  return settled;
}

/** The whole drive searched by name (`search_drives {drive_id}`), once typing settles: every match
 * outside the folder shown, which the filter above already covers, with its path. */
function DriveMatches({
  drive,
  folder,
  q,
  scope,
  open,
  wsPath,
}: {
  drive: Drive;
  folder: string;
  q: string;
  scope: { customerId: string; workspaceId: string };
  open: (key: string) => void;
  wsPath: (path: string) => string;
}) {
  const settled = useSettled(q, 250);
  const search = useQuery<SearchDrivesResult>({
    queryKey: ["drive-search", drive.id, scope.workspaceId, settled],
    queryFn: ({ signal }) => read<SearchDrivesResult>("search_drives", { customer_id: scope.customerId, workspace_id: scope.workspaceId, drive_id: drive.id, q: settled, limit: 100 }, { signal }),
    enabled: !!settled,
    staleTime: 15_000,
  });
  const elsewhere = (search.data?.items ?? []).filter((m: DriveMatch) => m.kind !== "drive" && parentKey(m) !== folder);
  const skipped = search.data?.skipped ?? [];
  return (
    <section className="mt-4 overflow-hidden rounded-[6px] border border-hairline bg-surface" aria-label="Matches in this drive">
      <header className="flex items-center gap-2 border-b border-hairline bg-raised px-4 py-2">
        <h2 className="!m-0 !text-[10.5px] after:!hidden">Elsewhere in This Drive</h2>
        <span className="ml-auto font-mono text-[11px] text-ink-3">{search.isLoading || settled !== q ? "…" : elsewhere.length}</span>
      </header>
      {search.isLoading || (settled !== q && !search.data) ? (
        <div className="px-4">
          <Skeleton lines={2} />
        </div>
      ) : search.isError ? (
        <div className="p-3">
          <ErrorBlock error={search.error} retry={() => void search.refetch()} what="the drive's search" />
        </div>
      ) : skipped.length ? (
        <p className="muted m-0 px-4 py-3 text-[13px]">This drive could not be searched: {skipped[0].reason}.</p>
      ) : !elsewhere.length ? (
        <p className="muted m-0 px-4 py-3 text-[13px]">No other folder of this drive holds a match.</p>
      ) : (
        <ul className="m-0 list-none p-0">
          {elsewhere.map((m) => {
            const { icon: Icon, className } = matchIcon(m);
            const name = <Highlight text={m.name} q={settled} />;
            return (
              <li key={`${m.kind}:${m.id ?? m.path}`} className="border-b border-hairline px-4 py-2 last:border-b-0 hover:bg-raised/60">
                <div className="flex min-w-0 items-center gap-2 text-[13.5px] font-medium">
                  <Icon size={14} aria-hidden className={cx("shrink-0", className)} />
                  {m.kind === "folder" ? (
                    <button type="button" className={LINK} onClick={() => open(folderKey(m))}>
                      {name}
                    </button>
                  ) : (
                    <MatchAnchor link={matchLink(m, scope)} wsPath={wsPath}>
                      {name}
                    </MatchAnchor>
                  )}
                </div>
                <div className="mt-0.5 flex flex-wrap items-center gap-x-2 text-[12.5px] text-ink-3">
                  <button type="button" className="cursor-pointer border-0 bg-transparent p-0 text-left text-ink-3 hover:underline" onClick={() => open(parentKey(m))}>
                    {matchWhere(m)}
                  </button>
                  {m.kind === "file" && m.size !== undefined && <span className="font-mono text-[11.5px]">{displaySize(m.size)}</span>}
                  {m.kind === "file" && m.modified_at && <span className="font-mono text-[11.5px]">{relative(m.modified_at)}</span>}
                </div>
              </li>
            );
          })}
          {search.data?.truncated && <li className="px-4 py-2 text-[12.5px] text-ink-3">More match; type more of the name to narrow the search.</li>}
        </ul>
      )}
    </section>
  );
}

/** The folder's path from the drive's top; each crumb opens its folder and takes dropped items. */
function PathBar({
  drive,
  crumbs,
  open,
  dropOn,
  dropTarget,
}: {
  drive: Drive;
  crumbs: { key: string; name: string }[];
  open: (key: string) => void;
  dropOn: (target: string) => Record<string, (event: DragEvent) => void>;
  dropTarget: string | null;
}) {
  const all = [{ key: "", name: drive.name }, ...crumbs];
  return (
    <nav aria-label="Folder path" className="flex flex-wrap items-center gap-1 text-[13px]">
      {all.map((crumb, index) => (
        <span key={crumb.key || "top"} className="inline-flex items-center gap-1">
          {index > 0 && <ChevronRight size={13} aria-hidden className="text-ink-3" />}
          {index === all.length - 1 ? (
            <span className="font-medium text-ink" aria-current="page">
              {crumb.name}
            </span>
          ) : (
            <button type="button" className={cx(LINK, dropTarget === crumb.key && "outline outline-2 outline-action")} onClick={() => open(crumb.key)} {...dropOn(crumb.key)}>
              {crumb.name}
            </button>
          )}
        </span>
      ))}
    </nav>
  );
}

/** An item's name: a folder opens in place, a file opens its view (or downloads), a shortcut opens
 * what it points at, or says it is broken. */
function ItemName({ item, storage, open, wsPath }: { item: Item; storage: DriveStorage; open: (key: string) => void; wsPath: (path: string) => string }) {
  if (item.kind === "folder") {
    return (
      <button type="button" className={cx(LINK, "flex min-w-0 items-center gap-2.5")} onClick={(e) => { e.stopPropagation(); open(item.key); }}>
        <Folder size={15} aria-hidden className="shrink-0 text-action" />
        <span className="truncate font-medium">{item.name}</span>
      </button>
    );
  }
  if (item.kind === "shortcut") {
    const href = targetHref(item.target);
    const label = (
      <>
        <Link2 size={15} aria-hidden className={cx("shrink-0", item.status === "broken" ? "text-bad" : "text-ai")} />
        <span className="truncate font-medium">{item.name}</span>
      </>
    );
    return (
      <span className="flex min-w-0 items-center gap-2.5">
        {!href || item.status === "broken" ? (
          <span className="flex min-w-0 items-center gap-2.5">{label}</span>
        ) : href.external ? (
          <a href={href.href} target="_blank" rel="noopener noreferrer" className="flex min-w-0 items-center gap-2.5" onClick={(e) => e.stopPropagation()}>
            {label}
            <ExternalLink size={12} aria-hidden className="shrink-0 text-ink-3" />
          </a>
        ) : (
          <Link to={href.workspace ? wsPath(href.href) : href.href} className="flex min-w-0 items-center gap-2.5" onClick={(e) => e.stopPropagation()}>
            {label}
          </Link>
        )}
        {item.status === "broken" && (
          <Chip tone="bad" title={item.reason}>
            <AlertTriangle size={11} aria-hidden /> Broken
          </Chip>
        )}
      </span>
    );
  }
  const view = storage.viewHref(item);
  const href = view ?? storage.downloadHref(item);
  const inLake = view?.startsWith("/drive/preview");
  return (
    <span className="flex min-w-0 items-center gap-2.5">
      <FileText size={15} aria-hidden className="shrink-0 text-ink-3" />
      {href && inLake ? (
        <Link to={href} className="truncate font-medium" onClick={(e) => e.stopPropagation()}>
          {item.name}
        </Link>
      ) : href ? (
        <a href={href} {...(view ? { target: "_blank", rel: "noopener noreferrer" } : { download: item.name })} className="truncate font-medium" onClick={(e) => e.stopPropagation()}>
          {item.name}
        </a>
      ) : (
        <span className="truncate font-medium">{item.name}</span>
      )}
    </span>
  );
}

/* ------------------------------------------------------------------ uploads */

/** Files chosen (a folder chosen whole keeps each file's path inside it) or dropped (folders
 * walked), sent to the storage with progress. */
function useUploader(storage: DriveStorage, folder: string) {
  const toast = useToast();
  const filesInput = useRef<HTMLInputElement>(null);
  const folderInput = useRef<HTMLInputElement>(null);
  const [busy, setBusy] = useState(false);
  const [progress, setProgress] = useState<Progress | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [refused, setRefused] = useState<string[]>([]);
  const send = async (items: UploadItem<File>[], into: string = folder) => {
    if (!items.length) return;
    setBusy(true);
    setError(null);
    setRefused([]);
    try {
      const report = await storage.upload(into, items, setProgress);
      setRefused(report.refused);
      refreshAfterWrite();
      void queryClient.invalidateQueries({ queryKey: ["drive-listing"] });
      toast(report.uploaded === 1 ? "1 file uploaded" : `${report.uploaded} files uploaded`);
    } catch (e) {
      setError((e as Error).message);
      refreshAfterWrite();
    } finally {
      setBusy(false);
      setProgress(null);
    }
  };
  return {
    busy,
    progress,
    error,
    refused,
    filesInput,
    folderInput,
    send,
    clear: () => {
      setError(null);
      setRefused([]);
    },
    chooseFiles: () => filesInput.current?.click(),
    chooseFolder: () => folderInput.current?.click(),
    fromInput: async (input: HTMLInputElement) => {
      const items = [...(input.files ?? [])].map((file) => ({ relativePath: file.webkitRelativePath || file.name, size: file.size, file }));
      input.value = "";
      await send(items);
    },
  };
}

/** What was dropped, folders walked: each file with its path inside what was dropped. */
async function droppedFiles(event: DragEvent): Promise<UploadItem<File>[]> {
  const out: UploadItem<File>[] = [];
  const entries = [...event.dataTransfer.items].map((item) => item.webkitGetAsEntry()).filter((entry): entry is FileSystemEntry => !!entry);
  if (!entries.length) {
    for (const file of [...event.dataTransfer.files]) out.push({ relativePath: file.name, size: file.size, file });
    return out;
  }
  const walk = async (entry: FileSystemEntry, prefix: string): Promise<void> => {
    const path = prefix ? `${prefix}/${entry.name}` : entry.name;
    if (entry.isFile) {
      const file = await new Promise<File>((ok, fail) => (entry as FileSystemFileEntry).file(ok, fail));
      out.push({ relativePath: path, size: file.size, file });
      return;
    }
    if (entry.isDirectory) {
      const reader = (entry as FileSystemDirectoryEntry).createReader();
      // readEntries answers a batch at a time, then an empty batch.
      for (;;) {
        const batch = await new Promise<FileSystemEntry[]>((ok, fail) => reader.readEntries(ok, fail));
        if (!batch.length) break;
        for (const child of batch) await walk(child, path);
      }
    }
  };
  for (const entry of entries) await walk(entry, "");
  return out;
}

/* ------------------------------------------------------------------ dialogs */

function NewFolder({ storage, folder, onClose }: { storage: DriveStorage; folder: string; onClose: () => void }) {
  const toast = useToast();
  const [name, setName] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const problem = name && !validName(name.trim()) ? "A name has no slash or backslash and is at most 255 bytes." : undefined;
  return (
    <Modal
      title="New Folder"
      onClose={onClose}
      width={440}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button type="submit" form="drive-new-folder" variant="primary" loading={busy} disabled={!name.trim() || !!problem}>
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
            await storage.createFolder(folder, name.trim());
            refreshAfterWrite();
            toast("Folder created");
            onClose();
          } catch (err) {
            setError(err);
          } finally {
            setBusy(false);
          }
        }}
      >
        {!fieldError(error, "name") && !fieldError(error, "path") && <FormError error={error} />}
        <Field label="Name" required error={problem ?? fieldError(error, "name") ?? fieldError(error, "path")}>
          <input value={name} onChange={(e) => setName(e.target.value)} autoFocus />
        </Field>
      </form>
    </Modal>
  );
}

function Rename({ storage, item, onClose }: { storage: DriveStorage; item: Item; onClose: () => void }) {
  const toast = useToast();
  const [name, setName] = useState(item.name);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const problem = name && !validName(name.trim()) ? "A name has no slash or backslash and is at most 255 bytes." : undefined;
  return (
    <Modal
      title={`Rename ${item.name}`}
      onClose={onClose}
      width={440}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button type="submit" form="drive-rename" variant="primary" loading={busy} disabled={!name.trim() || !!problem || name.trim() === item.name}>
            Rename
          </Button>
        </>
      }
    >
      <form
        id="drive-rename"
        className="stack"
        onSubmit={async (e) => {
          e.preventDefault();
          setBusy(true);
          setError(null);
          try {
            await storage.rename(item, name.trim());
            refreshAfterWrite();
            toast("Renamed");
            onClose();
          } catch (err) {
            setError(err);
          } finally {
            setBusy(false);
          }
        }}
      >
        <FormError error={error} />
        <Field label="New name" required error={problem}>
          <input value={name} onChange={(e) => setName(e.target.value)} autoFocus />
        </Field>
      </form>
    </Modal>
  );
}

/** Delete, saying what it means for this drive: a repository keeps history, workspace files do
 * not, and a folder goes with everything in it. */
function Delete({ storage, items, onClose, onDone }: { storage: DriveStorage; items: Item[]; onClose: () => void; onDone: () => void }) {
  const toast = useToast();
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const folders = items.some((item) => item.kind === "folder");
  const what = items.length === 1 ? `“${items[0].name}”` : `${items.length} items`;
  const consequence =
    storage.drive.storage === "repository"
      ? "This writes a new change without it. Earlier versions stay in the repository's history and can be restored."
      : items.every((item) => item.kind === "shortcut")
        ? "Only the shortcut goes; what it points at stays where it is."
        : `Workspace files have no history: this cannot be undone.${folders ? " A folder goes with everything in it." : ""}`;
  return (
    <Modal
      title={`Delete ${what}?`}
      onClose={onClose}
      width={460}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button
            variant="danger"
            loading={busy}
            onClick={async () => {
              setBusy(true);
              setError(null);
              try {
                for (const item of items) await storage.remove(item);
                refreshAfterWrite();
                toast(items.length === 1 ? "Deleted" : `${items.length} items deleted`);
                onDone();
                onClose();
              } catch (e) {
                refreshAfterWrite();
                setError((e as Error).message);
              } finally {
                setBusy(false);
              }
            }}
          >
            Delete
          </Button>
        </>
      }
    >
      <div className="muted">{consequence}</div>
      {error && (
        <div className="error mt-3" role="alert">
          {error}
        </div>
      )}
    </Modal>
  );
}

/** The drive itself: rename it, or delete it with its name typed. */
function DriveSettings({ drive, customerId, workspaceId, onClose, onDeleted }: { drive: Drive; customerId: string; workspaceId: string; onClose: () => void; onDeleted: () => void }) {
  const toast = useToast();
  const [name, setName] = useState(drive.name);
  const [typed, setTyped] = useState("");
  const [busy, setBusy] = useState<"rename" | "delete" | null>(null);
  const [error, setError] = useState<unknown>(null);
  const scope = { customer_id: customerId, workspace_id: workspaceId, id: drive.id };
  return (
    <Modal title="Drive Settings" subtitle={`${drive.name} · ${storageLabel(drive.storage)}`} onClose={onClose} width={520}>
      <div className="stack">
        <FormError error={error} />
        <form
          className="stack"
          onSubmit={async (e) => {
            e.preventDefault();
            setBusy("rename");
            setError(null);
            try {
              await act("rename_drive", { ...scope, name: name.trim() });
              toast("Drive renamed");
              onClose();
            } catch (err) {
              setError(err);
            } finally {
              setBusy(null);
            }
          }}
        >
          <Field label="Name" required>
            <input value={name} onChange={(e) => setName(e.target.value)} />
          </Field>
          <div>
            <Button type="submit" loading={busy === "rename"} disabled={!name.trim() || name.trim() === drive.name || !validName(name.trim())}>
              Rename drive
            </Button>
          </div>
        </form>
        <hr className="border-hairline" />
        <div>
          <h3 className="m-0 text-[14px] font-semibold">Delete This Drive</h3>
          <p className="muted mt-1">
            {drive.storage === "repository"
              ? "The drive goes; its repository stays, with every file and its history. Delete the repository from its own page."
              : drive.storage === "virtual"
                ? "Its folders and shortcuts go; what they point at stays where it is."
                : "Every folder and file in it is deleted and cannot be restored."}
          </p>
          <Field label={`Type “${drive.name}” to confirm`}>
            <input value={typed} onChange={(e) => setTyped(e.target.value)} />
          </Field>
          <Button
            variant="danger"
            loading={busy === "delete"}
            disabled={typed !== drive.name}
            onClick={async () => {
              setBusy("delete");
              setError(null);
              try {
                await act("delete_drive", { ...scope, confirm_name: typed });
                onDeleted();
              } catch (err) {
                setError(err);
              } finally {
                setBusy(null);
              }
            }}
          >
            Delete drive
          </Button>
        </div>
      </div>
    </Modal>
  );
}

/** Uploads begun here and never finished hold their names, so a new upload of the same name
 * would be renamed (lake#588). They are named here with their age, and can be removed; lake also
 * removes one 24 hours after it began, when another upload of its name arrives. */
function UnfinishedUploads({ storage, uploads }: { storage: DriveStorage; uploads: Upload[] }) {
  const toast = useToast();
  const [busy, setBusy] = useState(false);
  const age = (seconds: number) => (seconds < 3600 ? `${Math.max(1, Math.round(seconds / 60))} min` : `${Math.round(seconds / 3600)} h`);
  return (
    <Banner
      tone="quiet"
      title={uploads.length === 1 ? "1 Unfinished Upload" : `${uploads.length} Unfinished Uploads`}
      action={
        <Button
          variant="ghost"
          loading={busy}
          onClick={async () => {
            setBusy(true);
            try {
              for (const upload of uploads) await storage.remove({ key: upload.key, kind: "file", name: upload.name, path: upload.name, size: upload.size });
              refreshAfterWrite();
              toast(uploads.length === 1 ? "Unfinished upload removed" : "Unfinished uploads removed");
            } finally {
              setBusy(false);
            }
          }}
        >
          Remove
        </Button>
      }
    >
      {uploads.map((upload) => `${upload.name} (${upload.expired ? "expired" : `${age(upload.ageSeconds)} ago`})`).join(", ")}. An upload not finished
      holds its name until it is removed, or until 24 hours after it began.
    </Banner>
  );
}

/** Load as dataset (lake#593): a CSV or JSONL file of the drive becomes a dataset of the
 * workspace through load_drive_dataset, which reads the file and calls upload_dataset. */
function LoadDataset({ drive, item, customerId, workspaceId, onClose }: { drive: Drive; item: Item; customerId: string; workspaceId: string; onClose: () => void }) {
  const navigate = useNavigate();
  const wsPath = useWsPath();
  const [tableName, setTableName] = useState(datasetName(item.name));
  const [rename, setRename] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const format = datasetFormat(item.name) ?? "csv";
  return (
    <Modal
      title={`Load ${item.name} as a Dataset`}
      subtitle="The file's rows become a dataset of this workspace, for SQL and training."
      onClose={onClose}
      width={480}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button type="submit" form="drive-dataset" variant="primary" loading={busy} disabled={!/^[A-Za-z_][A-Za-z0-9_]{0,62}$/.test(tableName)}>
            Load dataset
          </Button>
        </>
      }
    >
      <form
        id="drive-dataset"
        className="stack"
        onSubmit={async (e) => {
          e.preventDefault();
          setBusy(true);
          setError(null);
          try {
            const result = await act("load_drive_dataset", {
              customer_id: customerId,
              workspace_id: workspaceId,
              drive_id: drive.id,
              id: item.key,
              table_name: tableName,
              format,
              if_exists: rename ? "rename" : "fail",
            });
            onClose();
            navigate(
              wsPath(
                `/train/data?${qs({
                  customer_id: customerId,
                  loaded: text(result, "table") || tableName,
                  rows: typeof result.rows === "number" ? result.rows : 0,
                  database: text(result, "database") || "datasets",
                  format: text(result, "format") || format,
                  renamed_from: text(result, "renamed_from"),
                })}`,
              ),
            );
          } catch (err) {
            setError(err);
          } finally {
            setBusy(false);
          }
        }}
      >
        <FormError error={error} />
        <Field label="Dataset name" required error={fieldError(error, "table_name")}>
          <input value={tableName} onChange={(e) => setTableName(e.target.value)} autoFocus />
          <small className="muted">Letters, digits and underscores, starting with a letter, so SQL can query it.</small>
        </Field>
        <label className="flex! items-start gap-2">
          <input type="checkbox" checked={rename} onChange={(e) => setRename(e.target.checked)} />
          <span>If the name is taken, save it under the next free name</span>
        </label>
        <Banner tone="quiet">
          {format === "csv" ? "A CSV" : "A JSONL file"} of at most 16 MiB. A CSV row whose field count differs from the header is refused with its line number.
        </Banner>
      </form>
    </Modal>
  );
}
