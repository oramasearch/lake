/** /share/:token/preview?ref=&path= (web_ui_drive_link_preview): a file opened through a bearer
 * link, previewed by lake's format registry, with no sign-in. */
import { Link, useParams, useSearchParams } from "react-router-dom";
import { Skeleton } from "@/lib/ui";
import { displaySize, parentPath, pathName, useForgeRead } from "./forge";
import { ShareError, sharedUrl, useShareTrail } from "./share";

type SharedPreview = { path: string; root_path: string; blob: string; size: number; format: string; media_type: string; html: string };

export default function SharePreviewPage() {
  const { token = "" } = useParams();
  const [params] = useSearchParams();
  const ref = params.get("ref") || "main";
  const path = params.get("path") ?? "";
  const preview = useForgeRead<SharedPreview>("preview_drive_share_link", { token, ref, path }, { retry: false });
  useShareTrail("shared file preview");
  if (preview.isLoading) {
    return (
      <div className="page-container">
        <Skeleton />
      </div>
    );
  }
  if (preview.isError || !preview.data) return <ShareError title="Shared folder" message={(preview.error as Error | null)?.message ?? "share link is not recognized"} />;
  const p = preview.data;
  return (
    <div className="page-container">
      <p className="eyebrow">shared file preview</p>
      <h1>{pathName(p.path)}</h1>
      <p className="sub">
        {p.format} · {displaySize(p.size)}
      </p>
      <div className="actions">
        <Link to={sharedUrl(token, ref, parentPath(p.path))}>Back to folder</Link>
        <a href={sharedUrl(token, ref, p.path, "download")} download>
          Download
        </a>
      </div>
      {/* The format registry's markup, escaped by lake (DriveFormatRegistry, src/api/drive.cpp). */}
      <div className="panel" dangerouslySetInnerHTML={{ __html: p.html }} />
    </div>
  );
}
