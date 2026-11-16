// SPDX-License-Identifier: MIT

/** /drive/conflict?repository_id=&ref=&path= (web_ui_drive_conflict): the preserved versions of a
 * conflicted Drive file: the explanation, what differs between them and the choice of which value
 * to keep, beside the region, both versions and the preserved values. A resolution returns to the
 * file's folder. */
import { useNavigate, useSearchParams } from "react-router-dom";
import { useToast } from "@/lib/toast";
import { Banner, Card, KeyValue, Split } from "@/lib/ui";
import {
  ConflictReasons,
  ConflictRegion,
  ConflictResolution,
  ConflictValues,
  conflictKindLabel,
  displaySize,
  DriveBreadcrumbs,
  DriveDiff,
  driveHref,
  ForgePage,
  ForgeStatus,
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
    <ForgePage
      above={<DriveBreadcrumbs repository={c.repository} refName={refName} path={parentPath(path)} />}
      title={`Two versions of ${c.conflict.region.path}`}
      repository={c.repository}
      tab="files"
      sub="Both versions are preserved. Compare them before choosing how to reconcile the file."
      actions={<ForgeStatus value={c.conflict.kind} />}
    >
      <Banner tone="info" title={conflictKindLabel(c.conflict.kind)}>
        {c.conflict.explanation}
      </Banner>
      <Split
        side={
          <>
            <Card label="Affected region">
              <p className="m-0 text-[13px]">
                <ConflictRegion region={c.conflict.region} />
              </p>
            </Card>
            {c.versions.map((version, index) => (
              <Card key={index} label={`Version ${index + 1}`}>
                <KeyValue
                  rows={[
                    ["Digest", <code className="break-all">{version.blob}</code>],
                    ["Size", displaySize(version.size)],
                  ]}
                />
              </Card>
            ))}
            <Card label="Preserved values">
              <ConflictValues conflict={c.conflict} />
            </Card>
          </>
        }
      >
        {c.conflict.reasons?.length ? (
          <Card>
            <ConflictReasons conflict={c.conflict} />
          </Card>
        ) : null}
        <Card label="What differs">
          {c.difference ? (
            <DriveDiff lines={c.difference.lines} />
          ) : (
            <p className="muted m-0">A structural preview is not registered for this format. The digests and sizes beside identify both preserved values.</p>
          )}
        </Card>
        <Card label="Resolve">
          <ConflictResolution
            repositoryId={id}
            refName={refName}
            conflict={c.conflict}
            done={() => {
              toast("Conflict resolved");
              navigate(driveHref("/drive", id, refName, parentPath(path)));
            }}
          />
        </Card>
      </Split>
    </ForgePage>
  );
}
