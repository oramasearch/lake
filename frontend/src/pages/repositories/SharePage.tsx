/** /share/:token?ref=&path= (web_ui_drive_link): a folder opened through a bearer link, with no
 * sign-in. Every read rechecks the link's scope, role, expiry and revocation. Files download from
 * lake's `/share/:token/download` route. */
import { Link, useParams, useSearchParams } from "react-router-dom";
import { Skeleton } from "@/lib/ui";
import { itemsOf, parentPath, pathName, useForgeRead, type TreeEntry } from "./forge";
import { ShareError, sharedUrl, useShareTrail } from "./share";

type Resolved = { path: string; root_path: string; items: TreeEntry[] };

export default function SharePage() {
  const { token = "" } = useParams();
  const [params] = useSearchParams();
  const ref = params.get("ref") || "main";
  // `path` is sent only when the address carries it: without it the link opens its own folder.
  const path = params.get("path");
  const resolved = useForgeRead<Resolved>("resolve_drive_share_link", { token, ref, ...(path !== null ? { path } : {}) }, { retry: false });
  useShareTrail("shared drive folder");
  if (resolved.isLoading) {
    return (
      <div className="page-container">
        <Skeleton />
      </div>
    );
  }
  if (resolved.isError || !resolved.data) return <ShareError title="Shared folder" message={(resolved.error as Error | null)?.message ?? "share link is not recognized"} />;
  const folder = resolved.data;
  const entries = itemsOf(folder);
  return (
    <div className="page-container">
      <p className="eyebrow">shared drive folder</p>
      <h1>{folder.path}</h1>
      <p className="sub">View – browse and download. Access ends when this link expires or is revoked. Downloaded copies are independent.</p>
      {folder.path !== folder.root_path && folder.root_path !== "" && (
        <div className="actions">
          <Link to={sharedUrl(token, ref, folder.root_path)}>Shared folder</Link>
          <Link to={sharedUrl(token, ref, parentPath(folder.path))}>Up one folder</Link>
        </div>
      )}
      <div className="table-wrap">
        <table>
          <thead>
            <tr>
              <th>Name</th>
              <th>Kind</th>
              <th>Actions</th>
            </tr>
          </thead>
          <tbody>
            {entries.map((entry) => {
              const isFolder = entry.kind === "tree";
              const isFile = entry.kind === "blob";
              const open = sharedUrl(token, ref, entry.path, isFolder ? "" : "preview");
              return (
                <tr key={entry.path}>
                  <td>{isFolder || isFile ? <Link to={open}>{pathName(entry.path)}</Link> : pathName(entry.path)}</td>
                  <td>{entry.kind}</td>
                  <td>
                    {isFile ? (
                      <a href={sharedUrl(token, ref, entry.path, "download")} download>
                        Download
                      </a>
                    ) : isFolder ? (
                      <Link to={open}>Open folder</Link>
                    ) : null}
                  </td>
                </tr>
              );
            })}
            {entries.length === 0 && (
              <tr>
                <td colSpan={3} className="empty">
                  This folder is empty.
                </td>
              </tr>
            )}
          </tbody>
        </table>
      </div>
    </div>
  );
}
