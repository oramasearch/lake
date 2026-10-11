/** A `search_drives` match's icon and link (lake#561): Data → Search and a drive's search box. */
import type { ReactNode } from "react";
import { Link } from "react-router-dom";
import { FileText, Folder, HardDrive, Link2, type LucideIcon } from "lucide-react";
import type { DriveMatch, MatchLink } from "./search";

export function matchIcon(match: DriveMatch): { icon: LucideIcon; className: string } {
  switch (match.kind) {
    case "drive":
      return { icon: HardDrive, className: "text-action" };
    case "folder":
      return { icon: Folder, className: "text-action" };
    case "shortcut":
      return { icon: Link2, className: match.status === "broken" ? "text-bad" : "text-ai" };
    default:
      return { icon: FileText, className: "text-ink-3" };
  }
}

/** An app page (a workspace page gets the workspace's prefix) or a URL the browser opens itself. */
export function MatchAnchor({ link, wsPath, className, children }: { link: MatchLink; wsPath: (path: string) => string; className?: string; children: ReactNode }) {
  if ("to" in link) {
    return (
      <Link to={link.workspace ? wsPath(link.to) : link.to} className={className}>
        {children}
      </Link>
    );
  }
  return (
    <a href={link.href} className={className} {...(link.newTab ? { target: "_blank", rel: "noopener noreferrer" } : {})} {...(link.download ? { download: "" } : {})}>
      {children}
    </a>
  );
}
