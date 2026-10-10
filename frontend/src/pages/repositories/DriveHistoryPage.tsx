/** /drive/history?repository_id=&ref=&path= (web_ui_drive_history): every change that touched a
 * Drive path. Two text versions can be compared; a version can be restored, which writes a new
 * change and rewrites nothing. */
import { useState } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { useSession } from "@/lib/session";
import { useToast } from "@/lib/toast";
import { Button } from "@/lib/ui";
import { dateTime } from "@/lib/format";
import { DriveBreadcrumbs, driveHref, forgeAct, ForgePage, itemsOf, parentPath, pending, ToRepositories, useForgeRead, useRepositoryTrail, type Repository } from "./forge";

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
  const [restoring, setRestoring] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  useRepositoryTrail(history.data?.repository, "File History");
  const wait = pending(history);
  if (wait) return wait;
  const h = history.data!;
  const from = left ?? versions[1]?.id ?? "";
  const to = right ?? versions[0]?.id ?? "";
  const restore = async (changeId: string) => {
    setRestoring(changeId);
    setError(null);
    try {
      await forgeAct("restore_drive_version", { repository_id: id, ref: refName, path, change_id: changeId });
      toast("Version restored");
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setRestoring(null);
    }
  };
  const rows = itemsOf(h);
  return (
    <ForgePage
      above={<DriveBreadcrumbs repository={h.repository} refName={refName} path={parentPath(path)} />}
      eyebrow="drive history"
      title={path}
      sub="Every version is a history query. Restoring writes a new change and does not rewrite prior versions."
    >
      <div className="actions">
        <Link to={driveHref("/drive", id, refName, parentPath(path))}>Back to folder</Link>
      </div>
      {error && (
        <p className="error" role="alert">
          {error}
        </p>
      )}
      <form
        onSubmit={(e) => {
          e.preventDefault();
          navigate(driveHref("/drive/history/diff", id, refName, path, { left: from, right: to }));
        }}
      >
        <div className="table-wrap">
          <table>
            <thead>
              <tr>
                <th>When</th>
                <th>Change</th>
                <th>What changed</th>
                <th>Compare from</th>
                <th>Compare to</th>
                <th>Restore</th>
              </tr>
            </thead>
            <tbody>
              {rows.map((change, index) => (
                <tr key={change.id + index}>
                  <td>{dateTime(change.occurred_at)}</td>
                  <td>
                    <code>{change.id}</code>
                    <br />
                    {change.message}
                  </td>
                  <td>
                    {(change.changes ?? []).join(", ")}
                    {change.new_conflicts?.length > 0 && (
                      <>
                        <br />
                        <span className="muted">Conflict records:</span>
                        <ul>
                          {change.new_conflicts.map((conflict) => (
                            <li key={conflict}>
                              <code>{conflict}</code>
                            </li>
                          ))}
                        </ul>
                      </>
                    )}
                  </td>
                  {change.new_blob ? (
                    <>
                      <td>
                        <input aria-label={`Compare from ${change.id}`} type="radio" name="left" value={change.id} checked={from === change.id} onChange={() => setLeft(change.id)} />
                      </td>
                      <td>
                        <input aria-label={`Compare to ${change.id}`} type="radio" name="right" value={change.id} checked={to === change.id} onChange={() => setRight(change.id)} />
                      </td>
                      <td>
                        {viewer.signed_in ? (
                          <Button loading={restoring === change.id} disabled={!!restoring} onClick={() => void restore(change.id)}>
                            Restore
                          </Button>
                        ) : (
                          "—"
                        )}
                      </td>
                    </>
                  ) : (
                    <>
                      <td>—</td>
                      <td>—</td>
                      <td>Deleted</td>
                    </>
                  )}
                </tr>
              ))}
              {rows.length === 0 && (
                <tr>
                  <td colSpan={6} className="empty">
                    No changes touch this path.
                  </td>
                </tr>
              )}
            </tbody>
          </table>
        </div>
        {versions.length >= 2 && (
          <p>
            <Button type="submit">Compare selected text versions</Button>
          </p>
        )}
      </form>
    </ForgePage>
  );
}
