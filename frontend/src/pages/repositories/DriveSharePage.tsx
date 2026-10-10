/** /drive/share?repository_id=&ref=&path=&workspace_id= (web_ui_drive_share): who can reach a
 * Drive folder now, the four ways to deliver it, and the folder's links. A folder's links are held
 * in one workspace of the repository's space: without `workspace_id` the page opens the current
 * workspace when it is one of them, else the only one, else asks (web_ui_drive_share_workspaces). */
import { useState } from "react";
import { Link, Navigate, useSearchParams } from "react-router-dom";
import { useSession } from "@/lib/session";
import { useToast } from "@/lib/toast";
import { Button, CopyBlock, Notice, Skeleton } from "@/lib/ui";
import { dateTime } from "@/lib/format";
import {
  driveHref,
  ErrorPage,
  forgeAct,
  ForgePage,
  pending,
  ToRepositories,
  useForgeRead,
  useRepositoryTrail,
  type PolicyObject,
  type Repository,
} from "./forge";
import { linkActions, linkState, type EffectiveAccess } from "./share";

type Workspace = { id: string; name: string; customer_id: string; desired?: string };

/** The live workspaces of the repository's space, where its folders' links can be held. The
 * space's `list_workspaces` when the viewer may read it, else the viewer's own view of that space. */
export function useShareWorkspaces(customerId: string | undefined) {
  const { viewer } = useSession();
  const listed = useForgeRead<{ workspaces: Workspace[] }>(customerId ? "list_workspaces" : null, { customer_id: customerId }, { retry: false });
  const fallback = viewer.spaces.find((space) => space.id === customerId)?.workspaces ?? [];
  const choices = listed.data ? listed.data.workspaces.filter((w) => w.desired !== "deleted") : fallback;
  return { choices, loading: listed.isLoading };
}

export default function DriveSharePage() {
  const [params] = useSearchParams();
  const id = params.get("repository_id") ?? "";
  if (!id) return <ToRepositories />;
  const ref = params.get("ref") || "main";
  const path = params.get("path") ?? "";
  const workspaceId = params.get("workspace_id") ?? "";
  return workspaceId ? <FolderSharing id={id} refName={ref} path={path} workspaceId={workspaceId} /> : <ChooseWorkspace id={id} refName={ref} path={path} />;
}

/** No workspace named: check the share right first (as the page did before choosing), then open
 * the current or only workspace, or list the choices. */
function ChooseWorkspace({ id, refName, path }: { id: string; refName: string; path: string }) {
  const { workspaceId } = useSession();
  const repository = useForgeRead<Repository>("get_repository", { id });
  const folder = useForgeRead<{ object: PolicyObject }>("drive_browse", { repository_id: id, ref: refName, path });
  const object = folder.data?.object;
  const permission = useForgeRead<{ allowed: Record<string, boolean> }>(
    object ? "my_permissions" : null,
    // A Drive folder is named by its repository, ref and path, resolved as the Drive capabilities
    // authorize it; a GET takes `actions` as a JSON array (docs/api/web-app.md).
    { object_type: "drive folder", object_id: id, ref: refName, path, actions: JSON.stringify(["share"]) },
    { retry: false },
  );
  const { choices, loading } = useShareWorkspaces(repository.data?.customer_id);
  useRepositoryTrail(repository.data, "Sharing");
  const wait = pending(repository, folder);
  if (wait) return wait;
  if (loading || permission.isLoading) {
    return (
      <div className="page-container">
        <Skeleton />
      </div>
    );
  }
  // my_permissions is a hint for the page; drive_effective_access still checks the right itself.
  if (permission.data && permission.data.allowed.share === false) return <ErrorPage code="unauthorized" message="principal is not allowed to share drive folder" />;
  const chosen = choices.find((w) => w.id === workspaceId)?.id ?? (choices.length === 1 ? choices[0].id : null);
  if (chosen) return <Navigate to={driveHref("/drive/share", id, refName, path, { workspace_id: chosen })} replace />;
  return (
    <ForgePage
      eyebrow="drive sharing"
      title={`Share ${path}`}
      sub="A folder's links are held in one of this space's workspaces. Choose the workspace whose links you want to see or create."
    >
      <ul className="stack">
        {choices.map((w) => (
          <li key={w.id}>
            <Link to={driveHref("/drive/share", id, refName, path, { workspace_id: w.id })}>{w.name}</Link>
          </li>
        ))}
        {choices.length === 0 && <li className="empty">There is no workspace here yet. Create one before making a link.</li>}
      </ul>
    </ForgePage>
  );
}

function FolderSharing({ id, refName, path, workspaceId }: { id: string; refName: string; path: string; workspaceId: string }) {
  const toast = useToast();
  const access = useForgeRead<EffectiveAccess>("drive_effective_access", { repository_id: id, workspace_id: workspaceId, ref: refName, path });
  const { choices } = useShareWorkspaces(access.data?.repository.customer_id);
  const [created, setCreated] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [revoking, setRevoking] = useState<string | null>(null);
  useRepositoryTrail(access.data?.repository, "Sharing");
  const wait = pending(access);
  if (wait) return wait;
  const a = access.data!;
  const heldIn = choices.find((w) => w.id === workspaceId)?.name ?? workspaceId;
  const others = choices.filter((w) => w.id !== workspaceId);
  const revoke = async (linkId: string) => {
    setRevoking(linkId);
    setError(null);
    try {
      await forgeAct("revoke_drive_share_link", { workspace_id: workspaceId, link_id: linkId });
      toast("Link revoked");
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setRevoking(null);
    }
  };
  return (
    <ForgePage
      eyebrow="drive sharing"
      title={`Share ${path}`}
      sub="Before sharing, review who can access this folder, what they can do, whether access is inherited, and whether the recipient gets a live object, a continuing link, or an independent copy."
    >
      {error && (
        <p className="error" role="alert">
          {error}
        </p>
      )}
      {created && (
        <Notice title="Copy this link now">
          <p className="m-0">It is shown once; lake keeps only its hash.</p>
          <CopyBlock value={new URL(created, window.location.origin).toString()} />
        </Notice>
      )}
      <h2>Who Can Access This Folder Now</h2>
      <p className="sub">This is the live policy result. It names people and organization identities, their actual actions, and whether each access path is inherited.</p>
      <div className="table-wrap">
        <table>
          <thead>
            <tr>
              <th>Who</th>
              <th>What they can do</th>
              <th>How access arrives</th>
            </tr>
          </thead>
          <tbody>
            {a.items.map((entry, index) => (
              <tr key={index}>
                <td>
                  <code>{entry.principal_taap_id}</code>
                </td>
                <td>
                  {entry.role}: {entry.actions.join(", ")}
                </td>
                <td>{entry.inherited ? "inherited" : "direct"}</td>
              </tr>
            ))}
            {a.items.length === 0 && (
              <tr>
                <td colSpan={3} className="empty">
                  No grants.
                </td>
              </tr>
            )}
          </tbody>
        </table>
      </div>
      <h2>Choose a Delivery</h2>
      <div className="grid">
        <DirectShare
          id={id}
          refName={refName}
          path={path}
          heading="People"
          text="A named person receives a live folder. Updates and moves continue to apply."
          principalLabel="Person's TAAP identity"
          roleLabel="What they can do"
          button="Share with person"
        />
        <DirectShare
          id={id}
          refName={refName}
          path={path}
          heading="Organization"
          text="A named organization identity receives the same live folder. People appear only when their identity has effective access above."
          principalLabel="Organization TAAP identity"
          roleLabel="What it can do"
          button="Share with organization"
        />
        <CreateLink id={id} refName={refName} path={path} workspaceId={workspaceId} onCreated={setCreated} />
        <section className="panel">
          <h3>Independent Copy</h3>
          <p>Downloaded bytes stop being a live object. They cannot be updated or recalled by changing access here.</p>
        </section>
      </div>
      <h2>Folder Share Management</h2>
      <p className="sub">A link is a continuing capability, not an attachment. Revoking it prevents its next open; copies already downloaded remain independent.</p>
      <p className="muted">
        Links held in workspace <strong>{heldIn}</strong>
        {others.length > 0 && (
          <>
            . Other workspaces:{" "}
            {others.map((w, index) => (
              <span key={w.id}>
                {index > 0 && " · "}
                <Link to={driveHref("/drive/share", id, refName, path, { workspace_id: w.id })}>{w.name}</Link>
              </span>
            ))}
          </>
        )}
      </p>
      <div className="table-wrap">
        <table>
          <thead>
            <tr>
              <th>Who can access</th>
              <th>What they can do</th>
              <th>Until when</th>
              <th>State</th>
              <th>
                <span className="sr-only">Actions</span>
              </th>
            </tr>
          </thead>
          <tbody>
            {a.links.map((link) => (
              <tr key={link.id}>
                <td>Anyone with this link</td>
                <td>{linkActions(link.role)}</td>
                <td>{dateTime(link.expires_at)}</td>
                <td>{linkState(link)}</td>
                <td>
                  <span className="flex flex-wrap items-center gap-2">
                    <Link to={driveHref("/drive/share/link", id, refName, path, { workspace_id: workspaceId, link_id: link.id })}>Details</Link>
                    <Button loading={revoking === link.id} disabled={!!revoking} onClick={() => void revoke(link.id)}>
                      Revoke link
                    </Button>
                  </span>
                </td>
              </tr>
            ))}
            {a.links.length === 0 && (
              <tr>
                <td colSpan={5} className="empty">
                  No links.
                </td>
              </tr>
            )}
          </tbody>
        </table>
      </div>
    </ForgePage>
  );
}

const ROLES = [
  ["read", "View — browse and download"],
  ["write", "Edit — upload, rename, move, and delete"],
  ["share", "Share — browse and create shares"],
] as const;

/** People and Organization (serve_drive_share_direct): a live grant on the folder. */
function DirectShare({
  id,
  refName,
  path,
  heading,
  text,
  principalLabel,
  roleLabel,
  button,
}: {
  id: string;
  refName: string;
  path: string;
  heading: string;
  text: string;
  principalLabel: string;
  roleLabel: string;
  button: string;
}) {
  const toast = useToast();
  const [principal, setPrincipal] = useState("");
  const [role, setRole] = useState("read");
  const [digest, setDigest] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  return (
    <section className="panel">
      <h3>{heading}</h3>
      <p>{text}</p>
      <form
        className="stack"
        onSubmit={async (e) => {
          e.preventDefault();
          setBusy(true);
          setError(null);
          try {
            await forgeAct("share_drive_folder", { repository_id: id, ref: refName, path, principal_taap_id: principal, role, attestation_digest: digest });
            setPrincipal("");
            setDigest("");
            toast("Folder shared");
          } catch (err) {
            setError((err as Error).message);
          } finally {
            setBusy(false);
          }
        }}
      >
        {error && (
          <p className="error" role="alert">
            {error}
          </p>
        )}
        <label>
          {principalLabel}
          <input value={principal} onChange={(e) => setPrincipal(e.target.value)} required />
        </label>
        <label>
          {roleLabel}
          <select value={role} onChange={(e) => setRole(e.target.value)}>
            {ROLES.map(([value, label]) => (
              <option key={value} value={value}>
                {label}
              </option>
            ))}
          </select>
        </label>
        <label>
          Attestation digest
          <input value={digest} onChange={(e) => setDigest(e.target.value)} required />
        </label>
        <div>
          <Button type="submit" variant="primary" loading={busy}>
            {button}
          </Button>
        </div>
      </form>
    </section>
  );
}

/** Link (serve_drive_share_link_create): a bearer link held in this workspace. Its URL is a
 * one-time secret, shown once above. */
function CreateLink({ id, refName, path, workspaceId, onCreated }: { id: string; refName: string; path: string; workspaceId: string; onCreated: (url: string) => void }) {
  const [role, setRole] = useState("read");
  const [expires, setExpires] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  return (
    <section className="panel">
      <h3>Link</h3>
      <p>Anyone holding the link can open this live folder until its expiry or revocation.</p>
      <form
        className="stack"
        onSubmit={async (e) => {
          e.preventDefault();
          setBusy(true);
          setError(null);
          try {
            const link = await forgeAct<{ url: string }>("create_drive_share_link", { repository_id: id, workspace_id: workspaceId, ref: refName, path, role, expires_at: expires });
            onCreated(link.url);
            setExpires("");
          } catch (err) {
            setError((err as Error).message);
          } finally {
            setBusy(false);
          }
        }}
      >
        {error && (
          <p className="error" role="alert">
            {error}
          </p>
        )}
        <label>
          What link holders can do
          <select value={role} onChange={(e) => setRole(e.target.value)}>
            <option value="read">View — browse and download</option>
            <option value="write">Write scope — capability-aware clients only</option>
          </select>
        </label>
        <label>
          Until when (UTC)
          <input value={expires} onChange={(e) => setExpires(e.target.value)} placeholder="2030-01-01T00:00:00Z" required />
        </label>
        <label>
          Passcode
          <input disabled placeholder="Not available for drive links" />
        </label>
        <p className="muted">Passcodes are not supported for drive-share links. No passcode is sent or stored.</p>
        <div>
          <Button type="submit" variant="primary" loading={busy}>
            Create link
          </Button>
        </div>
      </form>
    </section>
  );
}
