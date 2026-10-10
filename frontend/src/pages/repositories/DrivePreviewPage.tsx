/** /drive/preview?repository_id=&ref=&path= (web_ui_drive_preview): a Drive file previewed by
 * lake's format registry, beside the file's facts. The preview is the server's own escaped markup
 * (text, Markdown, CSV, JSON, or an image, PDF, video or audio element over a data URI), inserted as
 * the page did. */
import { useSearchParams } from "react-router-dom";
import { ArrowLeft, Download, History } from "lucide-react";
import { Button, Card, Chip, KeyValue, Split } from "@/lib/ui";
import { displaySize, DriveBreadcrumbs, driveHref, ForgePage, Identifier, parentPath, pathName, pending, ToRepositories, useForgeRead, useRepositoryTrail, type Repository } from "./forge";

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
      title={pathName(p.path)}
      repository={p.repository}
      tab="files"
      sub={
        <span className="inline-flex flex-wrap items-center gap-2">
          <Chip>{p.format}</Chip> {displaySize(p.size)} · <code>{p.path}</code>
        </span>
      }
      actions={
        <>
          <Button to={driveHref("/drive", id, refName, parentPath(p.path))} variant="ghost">
            <ArrowLeft size={14} aria-hidden /> Back to folder
          </Button>
          <Button to={driveHref("/drive/history", id, refName, p.path)}>
            <History size={14} aria-hidden /> History
          </Button>
          <Button variant="primary" href={driveHref("/drive/download", id, refName, p.path)}>
            <Download size={14} aria-hidden /> Download
          </Button>
        </>
      }
    >
      <Split
        side={
          <Card label="File">
            <KeyValue
              rows={[
                ["Format", <Chip>{p.format}</Chip>],
                ["Media type", <code>{p.media_type}</code>],
                ["Size", displaySize(p.size)],
                ["Digest", <Identifier value={p.blob} />],
                ["Ref", <code>{refName}</code>],
              ]}
            />
          </Card>
        }
      >
        {/* The format registry's markup, escaped by lake (DriveFormatRegistry, src/api/drive.cpp). */}
        <Card label="Preview">
          <div className="min-w-0 overflow-auto" dangerouslySetInnerHTML={{ __html: p.html }} />
        </Card>
      </Split>
    </ForgePage>
  );
}
