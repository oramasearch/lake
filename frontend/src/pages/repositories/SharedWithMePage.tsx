// SPDX-License-Identifier: MIT

/** /shared-with-me (web_ui_shared_with_me): live folders shared directly with the viewer's
 * identity, with their figures, a search and a filter by state. */
import { useState } from "react";
import { Link } from "react-router-dom";
import { FolderSymlink, FolderX, Share2 } from "lucide-react";
import { useTrail } from "@/lib/trail";
import { DataTable, Empty, FilterChips, Page, Query, SearchInput, Stat, StatRow, Status, Toolbar } from "@/lib/ui";
import { driveHref, Identifier, itemsOf, useForgeRead, type Repository } from "./forge";

type SharedFolder = { repository: Repository; ref: string; node_id: string; path: string; role: string; shared_by: string; available: boolean; actions: string[] };

export default function SharedWithMePage() {
  const folders = useForgeRead<{ items: SharedFolder[] }>("drive_shared_with_me", {});
  const [search, setSearch] = useState("");
  const [state, setState] = useState<"available" | "removed" | null>(null);
  useTrail([{ label: "Shared with Me", to: "/shared-with-me" }, { label: "drive" }], "Shared with Me");
  const all = itemsOf(folders.data);
  const available = all.filter((folder) => folder.available).length;
  const needle = search.trim().toLowerCase();
  const rows = all.filter(
    (folder) =>
      (!state || (state === "available") === folder.available) &&
      (!needle || `${folder.repository.name || folder.repository.id}/${folder.path} ${folder.shared_by}`.toLowerCase().includes(needle)),
  );
  return (
    <Page eyebrow="Data" title="Shared with Me" sub="Live folders shared directly with your identity. A move keeps the same folder identity; a downloaded copy remains independent.">
      <Query
        q={folders}
        what="shared folders"
        isEmpty={(value) => itemsOf(value).length === 0}
        empty={
          <Empty icon={Share2} title="Nothing shared with you yet">
            No folders are shared directly with you.
          </Empty>
        }
      >
        {() => (
          <>
            <StatRow cols={3}>
              <Stat label="Shared folders" value={all.length} icon={Share2} sub="shared with your identity" />
              <Stat label="Available" value={available} icon={FolderSymlink} sub="open now" />
              <Stat label="Removed" value={all.length - available} icon={FolderX} sub="no longer reachable" />
            </StatRow>
            <Toolbar>
              <SearchInput value={search} onChange={setSearch} placeholder="Search folders…" label="Search folders" />
              <FilterChips
                options={[
                  { id: "available", label: "Available", count: available },
                  { id: "removed", label: "Removed", count: all.length - available },
                ]}
                value={state}
                onChange={setState}
              />
            </Toolbar>
            <DataTable<SharedFolder>
              what="shared folders"
              caption="Folders shared with me"
              rows={rows}
              rowKey={(folder) => `${folder.repository.id}:${folder.node_id}`}
              empty={
                <Empty compact title="No folder matches">
                  Clear the search or the filter to see more.
                </Empty>
              }
              columns={[
                {
                  key: "folder",
                  label: "Folder",
                  sort: (folder) => `${folder.repository.name || folder.repository.id}/${folder.path}`,
                  render: (folder) => {
                    const label = `${folder.repository.name || folder.repository.id}/${folder.path}`;
                    return (
                      <div className="flex min-w-0 items-start gap-2.5">
                        <FolderSymlink size={15} aria-hidden className="mt-0.5 shrink-0 text-action" />
                        <div className="min-w-0">
                          {folder.available ? (
                            <Link className="font-semibold" to={driveHref("/drive", folder.repository.id, folder.ref || "main", folder.path)}>
                              {label}
                            </Link>
                          ) : (
                            <span className="font-semibold">{label}</span>
                          )}
                          <div className="text-[12px] text-ink-3">at {folder.ref || "main"}</div>
                        </div>
                      </div>
                    );
                  },
                },
                { key: "shared_by", label: "Shared by", render: (folder) => <Identifier value={folder.shared_by} /> },
                { key: "actions", label: "What you can do", render: (folder) => <span className="text-[12.5px]">{folder.actions.join(", ")}</span> },
                { key: "state", label: "State", sort: (folder) => (folder.available ? 0 : 1), render: (folder) => <Status value={folder.available ? "available" : "removed"} tone={folder.available ? "success" : "danger"} /> },
              ]}
            />
          </>
        )}
      </Query>
    </Page>
  );
}
