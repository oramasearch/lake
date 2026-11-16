// SPDX-License-Identifier: MIT

/** Data → Drives (`/w/<id>/drives`, lake#561, docs/ui/drives.md): two tabs. **Drives**: the
 * workspace's drives, each keeping its files in a repository (with history), as workspace files,
 * as shortcuts only, or in one of its buckets or filesystems (a storage drive, lake#656), and the
 * workspace files changed most recently; New drive opens a drawer. **Storage** (`?tab=storage`,
 * moved from Datasets in lake#656): the workspace's buckets, filesystems and volumes. An old
 * `#storage` link opens the Storage tab. */
import { useEffect, useMemo, useState } from "react";
import { Link, useLocation, useNavigate } from "react-router-dom";
import { Archive, Clock, FileText, FolderGit2, HardDrive, Link2, Plus, Search } from "lucide-react";
import { useRead, type Row } from "@/lib/api";
import { relative } from "@/lib/format";
import { Button, Card, Chip, DataTable, Empty, ErrorBlock, FilterChips, Page, Section, Skeleton, Stat, StatRow, SearchInput, Toolbar } from "@/lib/ui";
import { useToast } from "@/lib/toast";
import { WorkspaceGate } from "@/components/WorkspaceGate";
import { displaySize } from "@/pages/repositories/forge";
import { qs, useWorkspace, useWsPath } from "@/pages/train/workspace";
import { STORAGE, storageLabel, type Storage } from "./targets";
import type { Drive } from "./storage";
import { NewDrive } from "./NewDrive";
import { StorageTab } from "./StorageTab";
import { QueryTabs, useQueryTab } from "@/pages/design/parts";

const TABS = ["drives", "storage"] as const;

type Recent = { drive: { id: string; name: string }; entry: { id: string; name: string; path: string; size?: number; modified_at: string; parent_id: string | null } };

const ICONS = { repository: FolderGit2, files: HardDrive, virtual: Link2, store: Archive } as const;

export default function DrivesPage() {
  const { customerId, workspaceId } = useWorkspace();
  const wsPath = useWsPath();
  const navigate = useNavigate();
  const toast = useToast();
  const [creating, setCreating] = useState<false | { store?: Row }>(false);
  const location = useLocation();
  const tab = useQueryTab(TABS, "drives");
  // Storage moved here from Datasets (lake#656): an old `#storage` link opens its tab.
  useEffect(() => {
    if (location.hash !== "#storage" || tab === "storage") return;
    const next = new URLSearchParams(location.search);
    next.set("tab", "storage");
    navigate(`${location.pathname}?${next.toString()}`, { replace: true });
  }, [location.hash, location.pathname, location.search, tab, navigate]);
  const [filter, setFilter] = useState("");
  const [kind, setKind] = useState<Storage | null>(null);
  const scope = { customer_id: customerId, workspace_id: workspaceId };
  const ready = !!customerId && !!workspaceId;
  const drives = useRead<{ drives: Drive[] }>(ready ? "list_drives" : null, scope);
  const recent = useRead<{ items: Recent[] }>(ready ? "recent_drive_entries" : null, { ...scope, limit: 8 });
  const all = useMemo(() => drives.data?.drives ?? [], [drives.data]);
  const needle = filter.trim().toLowerCase();
  const shown = all.filter((d) => (!kind || d.storage === kind) && (!needle || d.name.toLowerCase().includes(needle)));
  const count = (storage: Storage) => all.filter((d) => d.storage === storage).length;
  const driveHref = (id: string, folder?: string | null) => wsPath(`/drives/drive?${qs({ id, folder: folder ?? "" })}`);

  const newButton = (
    <Button variant="primary" onClick={() => setCreating({})}>
      <Plus size={15} aria-hidden /> New drive
    </Button>
  );

  return (
    <Page eyebrow="Data" title="Drives" sub="Where this workspace keeps its files: repositories with history, workspace files, folders of shortcuts, and its buckets, filesystems and volumes." actions={tab === "drives" ? newButton : null}>
      <QueryTabs<(typeof TABS)[number]>
        value={tab}
        tabs={[
          { id: "drives", label: "Drives", count: drives.data ? all.length : undefined },
          { id: "storage", label: "Storage" },
        ]}
      />
      {tab === "storage" ? (
        <StorageTab drives={all} onOpenAsDrive={(store) => setCreating({ store })} />
      ) : (
      <WorkspaceGate returnTo="/drives">
        <StatRow cols={4}>
          <Stat label="Drives" icon={HardDrive} value={drives.isLoading ? "…" : all.length} sub="in this workspace" />
          <Stat label="Workspace files" icon={HardDrive} value={drives.isLoading ? "…" : count("files")} sub="drives" />
          <Stat label="Repositories" icon={FolderGit2} value={drives.isLoading ? "…" : count("repository")} sub="drives with history" />
          <Stat label="Virtual" icon={Link2} value={drives.isLoading ? "…" : count("virtual")} sub="drives of shortcuts" />
        </StatRow>
        <Toolbar>
          <SearchInput value={filter} onChange={setFilter} placeholder="Filter drives by name" label="Filter drives" />
          <FilterChips<Storage> options={STORAGE.map((s) => ({ id: s.id, label: s.short, count: count(s.id) }))} value={kind} onChange={setKind} />
        </Toolbar>
        {drives.isLoading ? (
          <Skeleton lines={3} />
        ) : drives.isError ? (
          <ErrorBlock error={drives.error} retry={() => void drives.refetch()} what="drives" />
        ) : !all.length ? (
          <Empty icon={HardDrive} title="No drives yet" action={newButton}>
            A drive holds files and folders. Keep them as workspace files, in a repository with history, or as shortcuts to what you already have.
          </Empty>
        ) : !shown.length ? (
          <Empty compact icon={Search} title="Nothing matches" action={<Button onClick={() => { setFilter(""); setKind(null); }}>Clear the filters</Button>}>
            No drive is named like “{filter}”{kind ? ` among ${storageLabel(kind).toLowerCase()} drives` : ""}.
          </Empty>
        ) : (
          <div className="grid grid-cols-1 gap-3 sm:grid-cols-2 xl:grid-cols-3">
            {shown.map((drive) => {
              const Icon = ICONS[drive.storage] ?? HardDrive;
              return (
                <Card key={drive.id} to={driveHref(drive.id)} label={storageLabel(drive.storage)} title={drive.name}>
                  <div className="flex items-center gap-2 text-[12.5px] text-ink-3">
                    <Icon size={14} aria-hidden />
                    <span className="truncate">{STORAGE.find((s) => s.id === drive.storage)?.tagline}</span>
                  </div>
                  <div className="mt-2 font-mono text-[11px] text-ink-3">created {relative(drive.created_at)}</div>
                </Card>
              );
            })}
          </div>
        )}
        <Section title="Recent Files" sub="Workspace files changed most recently, in every workspace-files drive.">
          {recent.isError ? (
            <ErrorBlock error={recent.error} retry={() => void recent.refetch()} what="recent files" />
          ) : (
            <DataTable<Recent>
              q={recent}
              what="recent files"
              caption="Recent files"
              rows={recent.data?.items}
              rowKey={(row) => row.entry.id}
              empty={
                <Empty compact icon={Clock} title="Nothing changed yet">
                  Files you upload to a workspace-files drive show here.
                </Empty>
              }
              columns={[
                {
                  key: "name",
                  label: "Name",
                  render: (row) => (
                    <span className="flex min-w-0 items-center gap-2.5">
                      <FileText size={15} aria-hidden className="shrink-0 text-ink-3" />
                      <span className="min-w-0">
                        <Link to={driveHref(row.drive.id, row.entry.parent_id)} className="block truncate font-medium">
                          {row.entry.name}
                        </Link>
                        {row.entry.path !== row.entry.name && <span className="block truncate text-[12px] text-ink-3">{row.entry.path}</span>}
                      </span>
                    </span>
                  ),
                },
                { key: "drive", label: "Drive", render: (row) => <Chip>{row.drive.name}</Chip> },
                { key: "size", label: "Size", numeric: true, render: (row) => <span className="font-mono text-[12px]">{displaySize(row.entry.size ?? 0)}</span> },
                { key: "modified", label: "Modified", render: (row) => <span className="font-mono text-[12px] text-ink-2">{relative(row.entry.modified_at)}</span> },
              ]}
            />
          )}
        </Section>
      </WorkspaceGate>
      )}
      {creating && (
        <NewDrive
          store={creating.store}
          onClose={() => setCreating(false)}
          onCreated={(drive) => {
            setCreating(false);
            toast(`Drive “${drive.name}” created`);
            navigate(driveHref(drive.id));
          }}
        />
      )}
    </Page>
  );
}
