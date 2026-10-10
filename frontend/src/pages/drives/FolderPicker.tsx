/** A dialog that browses a drive's folders to pick one (Move), or any item (a shortcut to an item
 * of another drive), through the drive's `DriveStorage`. */
import { useState } from "react";
import { useQuery } from "@tanstack/react-query";
import { ChevronRight, FileText, Folder, Link2 } from "lucide-react";
import { Modal } from "@/lib/overlay";
import { Button, Empty, ErrorBlock, Skeleton, cx } from "@/lib/ui";
import type { DriveStorage, Item, Listing } from "./storage";

export function FolderPicker({
  storage,
  start,
  title,
  action,
  exclude = new Set(),
  items = false,
  onClose,
  onPick,
}: {
  storage: DriveStorage;
  start: string;
  title: string;
  action: string;
  /** Folders that cannot be picked or opened (the folders being moved). */
  exclude?: Set<string>;
  /** Pick an item rather than a folder. */
  items?: boolean;
  onClose: () => void;
  onPick: (key: string, item?: Item) => void | Promise<void>;
}) {
  const [folder, setFolder] = useState(start);
  const [chosen, setChosen] = useState<Item | null>(null);
  const [busy, setBusy] = useState(false);
  const listing = useQuery<Listing>({
    queryKey: ["drive-listing", storage.drive.id, storage.drive.storage, "picker", folder],
    queryFn: ({ signal }) => storage.list(folder, signal),
  });
  const rows = (listing.data?.items ?? []).filter((item) => items || item.kind === "folder");
  const crumbs = [{ key: "", name: storage.drive.name }, ...(listing.data?.crumbs ?? [])];
  const go = (key: string) => {
    setChosen(null);
    setFolder(key);
  };
  return (
    <Modal
      title={title}
      subtitle={items ? "Open folders to find the item, then choose it." : "Open the folder to move into, then choose it."}
      onClose={onClose}
      width={520}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button
            variant="primary"
            loading={busy}
            disabled={items ? !chosen : exclude.has(folder)}
            onClick={async () => {
              setBusy(true);
              try {
                await onPick(items ? chosen!.key : folder, chosen ?? undefined);
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
      <nav aria-label="Folder path" className="mb-2 flex flex-wrap items-center gap-1 text-[13px]">
        {crumbs.map((crumb, index) => (
          <span key={crumb.key || "top"} className="inline-flex items-center gap-1">
            {index > 0 && <ChevronRight size={13} aria-hidden className="text-ink-3" />}
            <button type="button" className="cursor-pointer border-0 bg-transparent p-0 text-action hover:underline" onClick={() => go(crumb.key)}>
              {crumb.name}
            </button>
          </span>
        ))}
      </nav>
      {listing.isLoading ? (
        <Skeleton lines={4} />
      ) : listing.isError ? (
        <ErrorBlock error={listing.error} retry={() => void listing.refetch()} what="this folder" />
      ) : !rows.length ? (
        <Empty compact title={items ? "Nothing here" : "No folders here"}>
          {items ? "This folder is empty." : `${action} puts it in this folder.`}
        </Empty>
      ) : (
        <ul className="m-0 max-h-[50vh] list-none overflow-auto rounded-[6px] border border-hairline p-0" aria-label="Items">
          {rows.map((item) => {
            const blocked = exclude.has(item.key);
            const Icon = item.kind === "folder" ? Folder : item.kind === "shortcut" ? Link2 : FileText;
            return (
              <li key={item.key} className="border-b border-hairline last:border-b-0">
                <button
                  type="button"
                  disabled={blocked}
                  className={cx(
                    "flex w-full cursor-pointer items-center gap-2.5 border-0 bg-transparent px-3 py-2 text-left text-[13.5px] hover:bg-raised disabled:cursor-not-allowed disabled:opacity-50",
                    chosen?.key === item.key && "bg-action-soft",
                  )}
                  onClick={() => (item.kind === "folder" && !items ? go(item.key) : setChosen(item))}
                  onDoubleClick={() => item.kind === "folder" && !blocked && go(item.key)}
                >
                  <Icon size={15} aria-hidden className={item.kind === "folder" ? "text-action" : "text-ink-3"} />
                  <span className="min-w-0 flex-1 truncate">{item.name}</span>
                  {item.kind === "folder" && <ChevronRight size={14} aria-hidden className="text-ink-3" />}
                </button>
              </li>
            );
          })}
        </ul>
      )}
    </Modal>
  );
}
