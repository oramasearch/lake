/** /drive/preview?repository_id=&ref=&path= (web_ui_drive_preview): a Drive file previewed by
 * lake's format registry. The preview is the server's own escaped markup (text, Markdown, CSV,
 * JSON, or an image, PDF, video or audio element over a data URI), inserted as the page did. */
import { Link, useSearchParams } from "react-router-dom";
import { Badge } from "@/lib/ui";
import { displaySize, DriveBreadcrumbs, driveHref, ForgePage, parentPath, pending, ToRepositories, useForgeRead, useRepositoryTrail, type Repository } from "./forge";

type Preview = { repository: Repository; path: string; blob: string; size: number; format: string; media_type: string; html: string };

export default function DrivePreviewPage() {
  const [params] = useSearchParams();
  const id = params.get("repository_id") ?? "";
  if (!id) return <ToRepositories />;
  return <DrivePreview id={id} refName={params.get("ref") || "main"} path={params.get("path") ?? ""} />;
}

function DrivePreview({ id, refName, path }: { id: string; refName: string; path: string }) {
  const preview = useForgeRead<Preview>("drive_preview", { repository_id: id, ref: refName, path });
  useRepositoryTrail(preview.data?.repository, "Files");
  const wait = pending(preview);
  if (wait) return wait;
  const p = preview.data!;
  return (
    <ForgePage
      above={<DriveBreadcrumbs repository={p.repository} refName={refName} path={p.path} />}
      eyebrow="drive preview"
      title={p.path}
      sub={
        <>
          <Badge>{p.format}</Badge> {displaySize(p.size)} · digest <code>{p.blob}</code>
        </>
      }
    >
      <div className="actions">
        <Link to={driveHref("/drive", id, refName, parentPath(p.path))}>Back to folder</Link>
        <a href={driveHref("/drive/download", id, refName, p.path)} download>
          Download
        </a>
        <Link to={driveHref("/drive/history", id, refName, p.path)}>History</Link>
      </div>
      {/* The format registry's markup, escaped by lake (DriveFormatRegistry, src/api/drive.cpp). */}
      <div className="panel" dangerouslySetInnerHTML={{ __html: p.html }} />
    </ForgePage>
  );
}
