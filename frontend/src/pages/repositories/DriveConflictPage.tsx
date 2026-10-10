/** /drive/conflict?repository_id=&ref=&path= (web_ui_drive_conflict): the preserved versions of a
 * conflicted Drive file, what differs between them, and the choice of which value to keep. A
 * resolution returns to the file's folder. */
import { useNavigate, useSearchParams } from "react-router-dom";
import { useToast } from "@/lib/toast";
import {
  ConflictReasons,
  ConflictRegion,
  ConflictResolution,
  ConflictValues,
  conflictKindLabel,
  displaySize,
  DriveDiff,
  driveHref,
  ForgePage,
  parentPath,
  pending,
  ToRepositories,
  useForgeRead,
  useRepositoryTrail,
  type Conflict,
  type Repository,
} from "./forge";

type DriveConflict = {
  repository: Repository;
  path: string;
  conflict: Conflict;
  versions: { path: string; blob: string; size: number }[];
  difference?: { lines: { kind: string; text: string }[] };
};

export default function DriveConflictPage() {
  const [params] = useSearchParams();
  const id = params.get("repository_id") ?? "";
  if (!id) return <ToRepositories />;
  return <ConflictDetail id={id} refName={params.get("ref") || "main"} path={params.get("path") ?? ""} />;
}

function ConflictDetail({ id, refName, path }: { id: string; refName: string; path: string }) {
  const navigate = useNavigate();
  const toast = useToast();
  const conflict = useForgeRead<DriveConflict>("drive_conflict", { repository_id: id, ref: refName, path });
  useRepositoryTrail(conflict.data?.repository, "Conflict");
  const wait = pending(conflict);
  if (wait) return wait;
  const c = conflict.data!;
  return (
    <ForgePage eyebrow="drive conflict" title={`Two versions of ${c.conflict.region.path}`}>
      <p>
        <strong>{conflictKindLabel(c.conflict.kind)}</strong>
      </p>
      <p>{c.conflict.explanation}</p>
      <p className="sub">
        Affected region: <ConflictRegion region={c.conflict.region} />
      </p>
      <ConflictReasons conflict={c.conflict} />
      <p className="sub">Both versions are preserved. Compare them before choosing how to reconcile the file.</p>
      <div className="grid">
        {c.versions.map((version, index) => (
          <section key={index} className="panel">
            <h2>Version {index + 1}</h2>
            <dl>
              <dt>Digest</dt>
              <dd>
                <code>{version.blob}</code>
              </dd>
              <dt>Size</dt>
              <dd>{displaySize(version.size)}</dd>
            </dl>
          </section>
        ))}
      </div>
      <h2>What Differs</h2>
      {c.difference ? (
        <DriveDiff lines={c.difference.lines} />
      ) : (
        <div className="panel">A structural preview is not registered for this format. The digests and sizes above identify both preserved values.</div>
      )}
      <ConflictValues conflict={c.conflict} />
      <ConflictResolution
        repositoryId={id}
        refName={refName}
        conflict={c.conflict}
        done={() => {
          toast("Conflict resolved");
          navigate(driveHref("/drive", id, refName, parentPath(path)));
        }}
      />
    </ForgePage>
  );
}
