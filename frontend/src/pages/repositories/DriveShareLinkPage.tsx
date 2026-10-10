/** /drive/share/link?repository_id=&ref=&path=&workspace_id=&link_id= (web_ui_drive_share_link): one
 * folder link: who can use it, what it allows, when it stops, and revoking it. lake has no
 * `get_drive_share_link`, so the page finds it among the folder's links as the server did. */
import { useSearchParams } from "react-router-dom";
import { ArrowLeft } from "lucide-react";
import { ConfirmButton } from "@/lib/overlay";
import { useToast } from "@/lib/toast";
import { Banner, Button, Card, KeyValue, Split, Status } from "@/lib/ui";
import { dateTime } from "@/lib/format";
import { DriveBreadcrumbs, driveHref, ErrorPage, forgeAct, ForgePage, Identifier, pending, ToRepositories, useForgeRead, useRepositoryTrail } from "./forge";
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
  useRepositoryTrail(access.data?.repository, "Share Link");
  const wait = pending(access);
  if (wait) return wait;
  const link = access.data!.links.find((item) => item.id === linkId);
  if (!link) return <ErrorPage code="not_found" message="drive share link was not found" />;
  const state = linkState(link);
  const revoke = async () => {
    await forgeAct("revoke_drive_share_link", { workspace_id: workspaceId, link_id: linkId });
    toast("Link revoked");
  };
  return (
    <ForgePage
      above={<DriveBreadcrumbs repository={access.data!.repository} refName={refName} path={path} />}
      title={`Link for ${path || "/"}`}
      repository={access.data!.repository}
      tab="files"
      sub="Who can use the link, what it allows, when it stops, and what revocation changes."
      actions={
        <Button variant="ghost" to={driveHref("/drive/share", id, refName, path, { workspace_id: workspaceId })}>
          <ArrowLeft size={14} aria-hidden /> Back to folder shares
        </Button>
      }
    >
      <Split
        side={
          state === "active" ? (
            <Card tone="exception" label="Revoke this link">
              <p className="mt-0 text-[13px] text-ink-2">Revoking prevents this link from opening the live folder on its next use. It does not recall files someone already downloaded.</p>
              <ConfirmButton
                variant="danger"
                title="Revoke this link?"
                confirmLabel="Revoke link"
                confirm="The link stops opening the live folder on its next use. Files someone already downloaded stay with them."
                onConfirm={revoke}
              >
                Revoke link
              </ConfirmButton>
            </Card>
          ) : (
            <Banner tone="quiet">This link no longer opens the folder.</Banner>
          )
        }
      >
        <Card label="Link" title={<Status value={state} />}>
          <KeyValue
            rows={[
              ["Who can access", "Anyone holding this bearer link"],
              ["What they can do", linkActions(link.role)],
              ["Until when", dateTime(link.expires_at)],
              ["State", state],
              ["Workspace", <Identifier value={workspaceId} />],
            ]}
          />
        </Card>
      </Split>
    </ForgePage>
  );
}
