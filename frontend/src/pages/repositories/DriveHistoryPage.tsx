/** /drive/history?repository_id=&ref=&path= (web_ui_drive_history): every change that touched a
 * Drive path. Two text versions can be compared (pick "from" and "to"); a version can be restored,
 * which writes a new change and rewrites nothing. */
import { useState } from "react";
import { useNavigate, useSearchParams } from "react-router-dom";
import { ArrowLeft, Clock, GitCompareArrows, History as HistoryIcon, RotateCcw } from "lucide-react";
import { useSession } from "@/lib/session";
import { useToast } from "@/lib/toast";
import { ConfirmButton } from "@/lib/overlay";
import { Banner, Button, Chip, DataTable, Empty, Stat, StatRow, Toolbar } from "@/lib/ui";
import { dateTime, relative } from "@/lib/format";
import { DriveBreadcrumbs, driveHref, forgeAct, ForgePage, ForgeStatus, Identifier, itemsOf, parentPath, pathName, pending, ToRepositories, useForgeRead, useRepositoryTrail, type Repository } from "./forge";

type Version = { id: string; occurred_at: string; message: string; changes: string[]; new_blob: string; new_conflicts: string[] };
type History = { repository: Repository; path: string; items: Version[] };

export default function DriveHistoryPage() {
  const [params] = useSearchParams();
  const id = params.get("repository_id") ?? "";
  if (!id) return <ToRepositories />;
  return <DriveHistory id={id} refName={params.get("ref") || "main"} path={params.get("path") ?? ""} />;
}

function DriveHistory({ id, refName, path }: { id: string; refName: string; path: string }) {
  const { viewer } = useSession();
  const toast = useToast();
  const navigate = useNavigate();
  const history = useForgeRead<History>("drive_history", { repository_id: id, ref: refName, path });
  const versions = itemsOf(history.data).filter((change) => change.new_blob);
  // The first version is "to" and the second "from", as the page checked them.
  const [left, setLeft] = useState<string | null>(null);
  const [right, setRight] = useState<string | null>(null);
  useRepositoryTrail(history.data?.repository, "File History");
  const wait = pending(history);
  if (wait) return wait;
  const h = history.data!;
  const from = left ?? versions[1]?.id ?? "";
  const to = right ?? versions[0]?.id ?? "";
  const restore = async (changeId: string) => {
    await forgeAct("restore_drive_version", { repository_id: id, ref: refName, path, change_id: changeId });
    toast("Version restored");
  };
  const rows = itemsOf(h);
  const deletions = rows.length - versions.length;
  const compare = () => navigate(driveHref("/drive/history/diff", id, refName, path, { left: from, right: to }));
  return (
    <ForgePage
      above={<DriveBreadcrumbs repository={h.repository} refName={refName} path={parentPath(path)} />}
      title={`History of ${pathName(path) || "/"}`}
      repository={h.repository}
      tab="files"
      sub={
        <>
          <code>{path || "/"}</code>. Restoring writes a new change and does not rewrite prior versions.
        </>
      }
      actions={
        <Button to={driveHref("/drive", id, refName, parentPath(path))} variant="ghost">
          <ArrowLeft size={14} aria-hidden /> Back to folder
        </Button>
      }
    >
      {rows.length > 0 && (
        <StatRow cols={3}>
          <Stat label="Changes" value={rows.length} icon={HistoryIcon} sub="touch this path" />
          <Stat label="Versions" value={versions.length} icon={RotateCcw} sub={deletions ? `${deletions} deletions` : "restorable"} />
          <Stat label="Latest" value={relative(rows[0]?.occurred_at)} icon={Clock} sub={dateTime(rows[0]?.occurred_at)} />
        </StatRow>
      )}
      {versions.length >= 2 && (
        <Toolbar>
          <Banner tone="quiet">
            Pick a version to compare <strong>from</strong> and one to compare <strong>to</strong>, then compare them line by line.
          </Banner>
          <div className="ml-auto">
            <Button variant="primary" onClick={compare}>
              <GitCompareArrows size={14} aria-hidden /> Compare selected text versions
            </Button>
          </div>
        </Toolbar>
      )}
      <form
        onSubmit={(e) => {
          e.preventDefault();
          compare();
        }}
      >
        <DataTable<Version>
          what="history"
          caption="Versions of this path"
          rows={rows}
          rowKey={(change, index) => change.id + index}
          empty={
            <Empty icon={HistoryIcon} title="No history">
              No changes touch this path.
            </Empty>
          }
          columns={[
            {
              key: "change",
              label: "Change",
              render: (change) => (
                <div className="min-w-0">
                  <div className="font-semibold">{change.message || "Untitled change"}</div>
                  <div className="text-[12px] text-ink-3">
                    <Identifier value={change.id} /> · <span title={dateTime(change.occurred_at)}>{relative(change.occurred_at)}</span>
                  </div>
                </div>
              ),
            },
            {
              key: "what",
              label: "What changed",
              render: (change) => (
                <div>
                  <span className="flex flex-wrap gap-1">
                    {(change.changes ?? []).map((what) => (
                      <ForgeStatus key={what} value={what} />
                    ))}
                    {!change.new_blob && <Chip tone="bad">deleted</Chip>}
                  </span>
                  {change.new_conflicts?.length > 0 && (
                    <div className="mt-1 text-[12px]">
                      <span className="muted">Conflict records:</span>
                      <ul className="m-0 pl-4">
                        {change.new_conflicts.map((conflict) => (
                          <li key={conflict}>
                            <code>{conflict}</code>
                          </li>
                        ))}
                      </ul>
                    </div>
                  )}
                </div>
              ),
            },
            {
              key: "from",
              label: "From",
              render: (change) =>
                change.new_blob ? <input aria-label={`Compare from ${change.id}`} type="radio" name="left" value={change.id} checked={from === change.id} onChange={() => setLeft(change.id)} /> : "—",
            },
            {
              key: "to",
              label: "To",
              render: (change) =>
                change.new_blob ? <input aria-label={`Compare to ${change.id}`} type="radio" name="right" value={change.id} checked={to === change.id} onChange={() => setRight(change.id)} /> : "—",
            },
            {
              key: "restore",
              label: <span className="sr-only">Restore</span>,
              render: (change) =>
                !change.new_blob ? (
                  <span className="muted">Deleted</span>
                ) : viewer.signed_in ? (
                  <ConfirmButton
                    title="Restore this version?"
                    confirmLabel="Restore"
                    confirm={
                      <>
                        Writes a new change that puts <code>{path}</code> back as it was in <Identifier value={change.id} />. No earlier version is rewritten.
                      </>
                    }
                    onConfirm={() => restore(change.id)}
                  >
                    Restore
                  </ConfirmButton>
                ) : (
                  "—"
                ),
            },
          ]}
        />
      </form>
    </ForgePage>
  );
}
