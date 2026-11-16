// SPDX-License-Identifier: MIT

/** /drive/history/diff?repository_id=&ref=&path=&left=&right= (web_ui_drive_version_diff): two
 * preserved text versions of a Drive file, line by line. Neither is changed. */
import { useSearchParams } from "react-router-dom";
import { ArrowLeft, ArrowRight } from "lucide-react";
import { Button, Card, KeyValue } from "@/lib/ui";
import { displaySize, DriveBreadcrumbs, DriveDiff, driveHref, ForgePage, Identifier, parentPath, pathName, pending, ToRepositories, useForgeRead, useRepositoryTrail, type Repository } from "./forge";

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
  const added = d.lines.filter((line) => line.kind === "added").length;
  const removed = d.lines.filter((line) => line.kind === "removed").length;
  const side = (label: string, change: string, version: { size: number; blob: string }) => (
    <Card label={label}>
      <KeyValue
        rows={[
          ["Change", <Identifier value={change} />],
          ["Size", displaySize(version.size)],
          ["Digest", <Identifier value={version.blob} />],
        ]}
      />
    </Card>
  );
  return (
    <ForgePage
      above={<DriveBreadcrumbs repository={d.repository} refName={refName} path={parentPath(path)} />}
      title={`Compare ${pathName(path)}`}
      repository={d.repository}
      tab="files"
      sub="Two preserved text versions. Neither version is changed by this comparison."
      actions={
        <Button to={driveHref("/drive/history", id, refName, path)} variant="ghost">
          <ArrowLeft size={14} aria-hidden /> Back to history
        </Button>
      }
    >
      <div className="mb-4 grid grid-cols-1 items-stretch gap-3 md:grid-cols-[1fr_auto_1fr]">
        {side("From", left, d.left)}
        <div className="hidden items-center justify-center text-ink-3 md:flex">
          <ArrowRight size={16} aria-hidden />
        </div>
        {side("To", right, d.right)}
      </div>
      <Card
        label="Line by line"
        title={<code>{path}</code>}
        actions={
          <span className="font-mono text-[12px]">
            <span className="additions">+{added}</span> <span className="deletions">−{removed}</span>
          </span>
        }
      >
        <DriveDiff lines={d.lines} />
      </Card>
    </ForgePage>
  );
}
