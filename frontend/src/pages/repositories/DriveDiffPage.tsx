/** /drive/history/diff?repository_id=&ref=&path=&left=&right= (web_ui_drive_version_diff): two
 * preserved text versions of a Drive file, line by line. Neither is changed. */
import { Link, useSearchParams } from "react-router-dom";
import { displaySize, DriveDiff, driveHref, ForgePage, pending, ToRepositories, useForgeRead, useRepositoryTrail, type Repository } from "./forge";

type VersionDiff = { repository: Repository; left: { size: number; blob: string }; right: { size: number; blob: string }; lines: { kind: string; text: string }[] };

export default function DriveDiffPage() {
  const [params] = useSearchParams();
  const id = params.get("repository_id") ?? "";
  if (!id) return <ToRepositories />;
  return (
    <VersionComparison id={id} refName={params.get("ref") || "main"} path={params.get("path") ?? ""} left={params.get("left") ?? ""} right={params.get("right") ?? ""} />
  );
}

function VersionComparison({ id, refName, path, left, right }: { id: string; refName: string; path: string; left: string; right: string }) {
  const diff = useForgeRead<VersionDiff>("diff_drive_versions", { repository_id: id, ref: refName, path, left, right });
  useRepositoryTrail(diff.data?.repository, "Version Comparison");
  const wait = pending(diff);
  if (wait) return wait;
  const d = diff.data!;
  return (
    <ForgePage eyebrow="drive version comparison" title={path} sub="Two preserved text versions. Neither version is changed by this comparison.">
      <div className="actions">
        <Link to={driveHref("/drive/history", id, refName, path)}>Back to history</Link>
      </div>
      <div className="grid">
        <section className="panel">
          <h2>From</h2>
          <code>{left}</code>
          <p>
            {displaySize(d.left.size)} · <code>{d.left.blob}</code>
          </p>
        </section>
        <section className="panel">
          <h2>To</h2>
          <code>{right}</code>
          <p>
            {displaySize(d.right.size)} · <code>{d.right.blob}</code>
          </p>
        </section>
      </div>
      <DriveDiff lines={d.lines} />
    </ForgePage>
  );
}
