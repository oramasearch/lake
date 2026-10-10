/** /drive/share/link?repository_id=&ref=&path=&workspace_id=&link_id= (web_ui_drive_share_link): one
 * folder link: who can use it, what it allows, when it stops, and revoking it. lake has no
 * `get_drive_share_link`, so the page finds it among the folder's links as the server did. */
import { useState } from "react";
import { Link, useSearchParams } from "react-router-dom";
import { useToast } from "@/lib/toast";
import { Button } from "@/lib/ui";
import { dateTime } from "@/lib/format";
import { driveHref, ErrorPage, forgeAct, ForgePage, pending, ToRepositories, useForgeRead, useRepositoryTrail } from "./forge";
import { linkActions, linkState, type EffectiveAccess } from "./share";

export default function DriveShareLinkPage() {
  const [params] = useSearchParams();
  const id = params.get("repository_id") ?? "";
  if (!id) return <ToRepositories />;
  return (
    <ShareLinkDetail
      id={id}
      refName={params.get("ref") || "main"}
      path={params.get("path") ?? ""}
      workspaceId={params.get("workspace_id") ?? ""}
      linkId={params.get("link_id") ?? ""}
    />
  );
}

function ShareLinkDetail({ id, refName, path, workspaceId, linkId }: { id: string; refName: string; path: string; workspaceId: string; linkId: string }) {
  const toast = useToast();
  const access = useForgeRead<EffectiveAccess>("drive_effective_access", { repository_id: id, workspace_id: workspaceId, ref: refName, path });
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  useRepositoryTrail(access.data?.repository, "Share Link");
  const wait = pending(access);
  if (wait) return wait;
  const link = access.data!.links.find((item) => item.id === linkId);
  if (!link) return <ErrorPage code="not_found" message="drive share link was not found" />;
  const state = linkState(link);
  const revoke = async () => {
    setBusy(true);
    setError(null);
    try {
      await forgeAct("revoke_drive_share_link", { workspace_id: workspaceId, link_id: linkId });
      toast("Link revoked");
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setBusy(false);
    }
  };
  return (
    <ForgePage eyebrow="drive share" title={`Link for ${path}`} sub="This page answers who can use the link, what it allows, when it stops, and what revocation changes.">
      {error && (
        <p className="error" role="alert">
          {error}
        </p>
      )}
      <dl className="details">
        <dt>Who can access</dt>
        <dd>Anyone holding this bearer link</dd>
        <dt>What they can do</dt>
        <dd>{linkActions(link.role)}</dd>
        <dt>Until when</dt>
        <dd>{dateTime(link.expires_at)}</dd>
        <dt>State</dt>
        <dd>{state}</dd>
      </dl>
      {state === "active" ? (
        <section className="panel">
          <h2>Revoke This Link</h2>
          <p>Revoking prevents this link from opening the live folder on its next use. It does not recall files someone already downloaded.</p>
          <Button variant="danger" loading={busy} onClick={() => void revoke()}>
            Revoke link
          </Button>
        </section>
      ) : (
        <p className="notice">This link no longer opens the folder.</p>
      )}
      <Link to={driveHref("/drive/share", id, refName, path, { workspace_id: workspaceId })}>Back to folder shares</Link>
    </ForgePage>
  );
}
