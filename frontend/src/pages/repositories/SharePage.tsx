// SPDX-License-Identifier: MIT

/** /share/:token?ref=&path= (web_ui_drive_link): a folder opened through a bearer link, with no
 * sign-in, as a small file manager. Every read rechecks the link's scope, role, expiry and
 * revocation. Files download from lake's `/share/:token/download` route. */
import { useState } from "react";
import { Link, useParams, useSearchParams } from "react-router-dom";
import { ArrowUp, Download, FolderOpen, Home } from "lucide-react";
import { Banner, Button, DataTable, Empty, Page, SearchInput, Skeleton, Toolbar } from "@/lib/ui";
import { EntryIcon, itemsOf, parentPath, pathName, useForgeRead, type TreeEntry } from "./forge";
import { ShareError, sharedUrl, useShareTrail } from "./share";

type Resolved = { path: string; root_path: string; items: TreeEntry[] };

export default function SharePage() {
  const { token = "" } = useParams();
  const [params] = useSearchParams();
  const ref = params.get("ref") || "main";
  // `path` is sent only when the address carries it: without it the link opens its own folder.
  const path = params.get("path");
  const [filter, setFilter] = useState("");
  const resolved = useForgeRead<Resolved>("resolve_drive_share_link", { token, ref, ...(path !== null ? { path } : {}) }, { retry: false });
  useShareTrail("shared drive folder");
  if (resolved.isLoading) {
    return (
      <div className="page-container">
        <Skeleton />
      </div>
    );
  }
  if (resolved.isError || !resolved.data) return <ShareError title="Shared folder" message={(resolved.error as Error | null)?.message ?? "share link is not recognized"} />;
  const folder = resolved.data;
  const entries = itemsOf(folder);
  const needle = filter.trim().toLowerCase();
  const shown = needle ? entries.filter((entry) => pathName(entry.path).toLowerCase().includes(needle)) : entries;
  const nested = folder.path !== folder.root_path && folder.root_path !== "";
  return (
    <Page
      eyebrow="Shared drive folder"
      title={pathName(folder.path) || folder.path || "/"}
      sub={<code>{folder.path || "/"}</code>}
      actions={
        nested ? (
          <>
            <Button to={sharedUrl(token, ref, folder.root_path)}>
              <Home size={14} aria-hidden /> Shared folder
            </Button>
            <Button to={sharedUrl(token, ref, parentPath(folder.path))}>
              <ArrowUp size={14} aria-hidden /> Up one folder
            </Button>
          </>
        ) : undefined
      }
    >
      <Banner tone="quiet">View – browse and download. Access ends when this link expires or is revoked. Downloaded copies are independent.</Banner>
      {entries.length > 0 && (
        <Toolbar>
          <SearchInput value={filter} onChange={setFilter} placeholder="Filter by name…" label="Filter by name" />
          <span className="ml-auto font-mono text-[12px] text-ink-3">
            {shown.length} of {entries.length} items
          </span>
        </Toolbar>
      )}
      <DataTable<TreeEntry>
        what="shared folder"
        caption="Shared folder contents"
        rows={shown}
        rowKey={(entry) => entry.path}
        empty={
          entries.length === 0 ? (
            <Empty icon={FolderOpen} title="This folder is empty" />
          ) : (
            <Empty compact title="Nothing matches">
              No item here is named like “{filter}”.
            </Empty>
          )
        }
        columns={[
          {
            key: "name",
            label: "Name",
            render: (entry) => {
              const isFolder = entry.kind === "tree";
              const isFile = entry.kind === "blob";
              const open = sharedUrl(token, ref, entry.path, isFolder ? "" : "preview");
              return (
                <span className="flex min-w-0 items-center gap-2.5">
                  <EntryIcon kind={entry.kind} />
                  {isFolder || isFile ? <Link to={open}>{pathName(entry.path)}</Link> : pathName(entry.path)}
                </span>
              );
            },
          },
          { key: "kind", label: "Kind", render: (entry) => <span className="text-[12.5px] text-ink-2">{entry.kind}</span> },
          {
            key: "actions",
            label: <span className="sr-only">Actions</span>,
            className: "text-right",
            render: (entry) =>
              entry.kind === "blob" ? (
                <Button variant="ghost" href={sharedUrl(token, ref, entry.path, "download")}>
                  <Download size={14} aria-hidden /> Download
                </Button>
              ) : entry.kind === "tree" ? (
                <Button variant="ghost" to={sharedUrl(token, ref, entry.path, "")}>
                  Open folder
                </Button>
              ) : null,
          },
        ]}
      />
    </Page>
  );
}
