// SPDX-License-Identifier: MIT

/** /share/:token/preview?ref=&path= (web_ui_drive_link_preview): a file opened through a bearer
 * link, previewed by lake's format registry, with no sign-in. */
import { useParams, useSearchParams } from "react-router-dom";
import { ArrowLeft, Download } from "lucide-react";
import { Button, Card, Chip, Page, Skeleton } from "@/lib/ui";
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
    <Page
      eyebrow="Shared file preview"
      title={pathName(p.path)}
      sub={
        <span className="inline-flex flex-wrap items-center gap-2">
          <Chip>{p.format}</Chip> {displaySize(p.size)}
        </span>
      }
      actions={
        <>
          <Button variant="ghost" to={sharedUrl(token, ref, parentPath(p.path))}>
            <ArrowLeft size={14} aria-hidden /> Back to folder
          </Button>
          <Button variant="primary" href={sharedUrl(token, ref, p.path, "download")}>
            <Download size={14} aria-hidden /> Download
          </Button>
        </>
      }
    >
      {/* The format registry's markup, escaped by lake (DriveFormatRegistry, src/api/drive.cpp). */}
      <Card label="Preview">
        <div className="min-w-0 overflow-auto" dangerouslySetInnerHTML={{ __html: p.html }} />
      </Card>
    </Page>
  );
}
