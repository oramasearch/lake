/** The drive picker (lake#682, docs/ui/drives.md#the-picker): a dialog that browses a drive's
 * folders, through the drive's `DriveStorage`, to pick a folder (Move) or an item (a shortcut to an
 * item of another drive; a model file to bring). With `drives` it starts from every drive of the
 * workspace, of any kind, and the person opens one. With `scope` it has a search box: what is typed
 * is searched by name (`search_drives`) in the open drive, or in every drive from the top, and a
 * match is opened or chosen as a listed item is, so an item at any depth is a search away. */
import { useMemo, useState } from "react";
import { useQuery } from "@tanstack/react-query";
import { Check, ChevronRight, FileText, Folder, HardDrive, Link2 } from "lucide-react";
import { read } from "@/lib/api";
import { Modal } from "@/lib/overlay";
import { Button, Empty, ErrorBlock, SearchInput, Skeleton, cx } from "@/lib/ui";
import { storageLabel } from "./targets";
import { choosable, matchDrive, matchItem, pickerRows, searchRows, type PickMode, type PickerItem } from "./picker";
import { matchWhere, type DriveMatch, type SearchDrivesResult } from "./search";
import { storageFor, type Drive, type DriveStorage, type Item, type Listing, type Scope } from "./storage";
import { useSettled } from "./useSettled";

type Chosen = { item: Item; storage: DriveStorage };

/** The chosen row's fill. Inline, because the dialogs' base button rule outranks a utility class. */
const CHOSEN = { background: "var(--lake-action-soft)" } as const;

export function FolderPicker({
  storage,
  drives,
  scope,
  start = "",
  title,
  subtitle,
  action,
  exclude = new Set(),
  items = false,
  accept,
  onClose,
  onPick,
}: {
  /** The drive to browse; with `drives` too, the drive it opens at first. */
  storage?: DriveStorage;
  /** Every drive the person may open, listed at the picker's top. */
  drives?: Drive[];
  /** The workspace, for search; without it the picker has no search box. */
  scope?: Scope;
  start?: string;
  title: string;
  subtitle?: string;
  action: string;
  /** Folders that cannot be picked or opened (the folders being moved). */
  exclude?: Set<string>;
  /** Pick an item rather than a folder. */
  items?: boolean;
  /** With `items`, the items that can be chosen; the others are not listed (folders always are). */
  accept?: (item: PickerItem) => boolean;
  onClose: () => void;
  onPick: (key: string, item: Item | undefined, storage: DriveStorage) => void | Promise<void>;
}) {
  const [current, setCurrent] = useState<DriveStorage | null>(storage ?? null);
  const [folder, setFolder] = useState(start);
  const [chosen, setChosen] = useState<Chosen | null>(null);
  const [busy, setBusy] = useState(false);
  const [q, setQ] = useState("");
  const settled = useSettled(q.trim(), 250);
  const mode: PickMode = useMemo(() => (items ? { kind: "item", accept } : { kind: "folder" }), [items, accept]);

  const listing = useQuery<Listing>({
    queryKey: ["drive-listing", current?.drive.id, current?.drive.storage, "picker", folder],
    queryFn: ({ signal }) => current!.list(folder, signal),
    enabled: !!current,
  });
  const search = useQuery<SearchDrivesResult>({
    queryKey: ["drive-search", current?.drive.id ?? "", scope?.workspaceId, settled, "picker"],
    queryFn: ({ signal }) =>
      read<SearchDrivesResult>(
        "search_drives",
        { customer_id: scope!.customerId, workspace_id: scope!.workspaceId, q: settled, limit: 100, ...(current ? { drive_id: current.drive.id } : {}) },
        { signal },
      ),
    enabled: !!scope && !!settled,
    staleTime: 15_000,
  });

  const open = (next: DriveStorage | null, key: string) => {
    setQ("");
    setCurrent(next);
    setFolder(key);
  };
  const storageOf = (drive: Drive): DriveStorage | null => (current?.drive.id === drive.id ? current : storage?.drive.id === drive.id ? storage : scope ? storageFor(drive, scope) : null);
  const choose = (item: Item, at: DriveStorage) => setChosen(chosen?.item.key === item.key && chosen.storage.drive.id === at.drive.id ? null : { item, storage: at });

  const rows = pickerRows(listing.data?.items ?? [], mode);
  const crumbs: { key: string; name: string; top?: boolean }[] = [
    ...(drives ? [{ key: "", name: "Drives", top: true }] : []),
    ...(current ? [{ key: "", name: current.drive.name }, ...(listing.data?.crumbs ?? [])] : []),
  ];
  const searching = !!scope && !!settled;
  const matches = searchRows(search.data?.items ?? [], mode);
  const pickable = items ? !!chosen : !!current && !exclude.has(folder);
  const isChosen = (key: string, driveId: string) => !!chosen && chosen.item.key === key && chosen.storage.drive.id === driveId;

  return (
    <Modal
      title={title}
      subtitle={subtitle ?? (items ? "Open folders to find the item, or search for it, then choose it." : "Open the folder to move into, then choose it.")}
      onClose={onClose}
      width={560}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button
            variant="primary"
            loading={busy}
            disabled={!pickable}
            onClick={async () => {
              setBusy(true);
              try {
                if (items) await onPick(chosen!.item.key, chosen!.item, chosen!.storage);
                else await onPick(folder, undefined, current!);
              } finally {
                setBusy(false);
              }
            }}
          >
            {action}
          </Button>
        </>
      }
    >
      {scope && (
        <div className="mb-2 grid">
          <SearchInput value={q} onChange={setQ} placeholder={current ? `Search ${current.drive.name}…` : "Search every drive…"} label={current ? `Search ${current.drive.name}` : "Search every drive"} />
        </div>
      )}
      <nav aria-label="Folder path" className="mb-2 flex flex-wrap items-center gap-1 text-[13px]">
        {crumbs.map((crumb, index) => (
          <span key={`${crumb.top ? "drives" : "drive"}:${crumb.key || "top"}`} className="inline-flex items-center gap-1">
            {index > 0 && <ChevronRight size={13} aria-hidden className="text-ink-3" />}
            <button type="button" className="cursor-pointer border-0 bg-transparent p-0 text-action hover:underline" onClick={() => (crumb.top ? open(null, "") : open(current, crumb.key))}>
              {crumb.name}
            </button>
          </span>
        ))}
      </nav>
      {items && chosen && (
        <p className="muted m-0 mb-2 truncate text-[12.5px]" aria-live="polite">
          Chosen: {chosen.storage.drive.name} / {chosen.item.path || chosen.item.name}
        </p>
      )}
      {searching ? (
        search.isLoading ? (
          <Skeleton lines={4} />
        ) : search.isError ? (
          <ErrorBlock error={search.error} retry={() => void search.refetch()} what="the search" />
        ) : !matches.length ? (
          <Empty compact title="No match">
            Nothing {current ? `in ${current.drive.name}` : "in any drive"} that can be chosen here is named like “{settled}”.
          </Empty>
        ) : (
          <ul className="m-0 max-h-[50vh] list-none overflow-auto rounded-[6px] border border-hairline p-0" aria-label="Matches">
            {matches.map((match) => {
              const at = storageOf(matchDrive(match));
              return (
                <MatchRow
                  key={`${match.drive.id}:${match.kind}:${match.id ?? match.path}`}
                  match={match}
                  mode={mode}
                  chosen={isChosen(matchItem(match).key, match.drive.id)}
                  onOpen={(key) => at && open(at, key)}
                  onChoose={(item) => at && choose(item, at)}
                />
              );
            })}
            {search.data?.truncated && <li className="muted px-3 py-2 text-[12.5px]">More match; type more of the name.</li>}
          </ul>
        )
      ) : !current ? (
        <DriveRows drives={drives ?? []} onOpen={(drive) => open(storageOf(drive), "")} />
      ) : listing.isLoading ? (
        <Skeleton lines={4} />
      ) : listing.isError ? (
        <ErrorBlock error={listing.error} retry={() => void listing.refetch()} what="this folder" />
      ) : !rows.length ? (
        <Empty compact title={items ? "Nothing here" : "No folders here"}>
          {items ? (accept ? "Nothing in this folder can be chosen here." : "This folder is empty.") : `${action} puts it in this folder.`}
        </Empty>
      ) : (
        <ul className="m-0 max-h-[50vh] list-none overflow-auto rounded-[6px] border border-hairline p-0" aria-label="Items">
          {rows.map((item) => {
            const blocked = exclude.has(item.key);
            const Icon = item.kind === "folder" ? Folder : item.kind === "shortcut" ? Link2 : FileText;
            const canChoose = items && choosable(item, mode);
            const isFolder = item.kind === "folder";
            return (
              <li key={item.key} className="border-b border-hairline last:border-b-0">
                <button
                  type="button"
                  disabled={blocked}
                  className={cx(
                    "flex min-h-[40px] w-full cursor-pointer items-center gap-2.5 border-0 px-3 py-2 text-left text-[13.5px] disabled:cursor-not-allowed disabled:opacity-50",
                    isChosen(item.key, current.drive.id) ? "bg-action-soft font-semibold" : "bg-transparent hover:bg-raised",
                  )}
                  aria-pressed={canChoose ? isChosen(item.key, current.drive.id) : undefined}
                  style={isChosen(item.key, current.drive.id) ? CHOSEN : undefined}
                  onClick={() => (isFolder && !(items && !accept) ? open(current, item.key) : choose(item, current))}
                  onDoubleClick={() => isFolder && !blocked && open(current, item.key)}
                >
                  <Icon size={15} aria-hidden className={isFolder ? "text-action" : "text-ink-3"} />
                  <span className="min-w-0 flex-1 truncate">{item.name}</span>
                  {isChosen(item.key, current.drive.id) && <Check size={14} aria-label="Chosen" className="text-action" />}
                  {isFolder && <ChevronRight size={14} aria-hidden className="text-ink-3" />}
                </button>
              </li>
            );
          })}
        </ul>
      )}
    </Modal>
  );
}

/** The picker's top: every drive, with its kind. */
function DriveRows({ drives, onOpen }: { drives: Drive[]; onOpen: (drive: Drive) => void }) {
  if (!drives.length)
    return (
      <Empty compact title="No drive yet">
        This workspace has no drive. Make one in Data → Drives.
      </Empty>
    );
  return (
    <ul className="m-0 max-h-[50vh] list-none overflow-auto rounded-[6px] border border-hairline p-0" aria-label="Drives">
      {drives.map((drive) => (
        <li key={drive.id} className="border-b border-hairline last:border-b-0">
          <button type="button" className="flex min-h-[40px] w-full cursor-pointer items-center gap-2.5 border-0 bg-transparent px-3 py-2 text-left text-[13.5px] hover:bg-raised" onClick={() => onOpen(drive)}>
            <HardDrive size={15} aria-hidden className="text-action" />
            <span className="min-w-0 flex-1 truncate">{drive.name}</span>
            <span className="muted shrink-0 text-[12px]">{storageLabel(drive.storage)}</span>
            <ChevronRight size={14} aria-hidden className="text-ink-3" />
          </button>
        </li>
      ))}
    </ul>
  );
}

/** One search match: a drive or folder opens; an item that can be chosen is chosen. */
function MatchRow({ match, chosen, mode, onOpen, onChoose }: { match: DriveMatch; chosen: boolean; mode: PickMode; onOpen: (key: string) => void; onChoose: (item: Item) => void }) {
  const item = matchItem(match) as Item;
  const opens = match.kind === "drive" || match.kind === "folder";
  const canChoose = mode.kind === "item" && choosable(item, mode) && !(opens && mode.accept);
  const Icon = match.kind === "drive" ? HardDrive : match.kind === "folder" ? Folder : match.kind === "shortcut" ? Link2 : FileText;
  return (
    <li className="border-b border-hairline last:border-b-0">
      <button
        type="button"
        className={cx("flex min-h-[44px] w-full cursor-pointer items-center gap-2.5 border-0 px-3 py-2 text-left text-[13.5px]", chosen ? "bg-action-soft font-semibold" : "bg-transparent hover:bg-raised")}
        aria-pressed={canChoose ? chosen : undefined}
        style={chosen ? CHOSEN : undefined}
        onClick={() => (canChoose ? onChoose(item) : onOpen(match.kind === "drive" ? "" : item.key))}
      >
        <Icon size={15} aria-hidden className={opens ? "text-action" : "text-ink-3"} />
        <span className="min-w-0 flex-1">
          <span className="block truncate">{match.name}</span>
          <span className="muted block truncate text-[12px]">{matchWhere(match)}</span>
        </span>
        {chosen && <Check size={14} aria-label="Chosen" className="text-action" />}
        {!canChoose && <ChevronRight size={14} aria-hidden className="text-ink-3" />}
      </button>
    </li>
  );
}
