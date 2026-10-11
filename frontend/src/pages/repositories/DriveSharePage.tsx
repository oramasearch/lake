/** /drive/share?repository_id=&ref=&path=&workspace_id= (web_ui_drive_share): who can reach a
 * Drive folder now (with its figures), the four ways to deliver it (each form in a dialog), and the
 * folder's links. A folder's links are held
 * in one workspace of the repository's space: without `workspace_id` the page opens the current
 * workspace when it is one of them, else the only one, else asks (web_ui_drive_share_workspaces). */
import { useState } from "react";
import { Link, Navigate, useSearchParams } from "react-router-dom";
import { Ban, Download, Layers, Link2, ShieldCheck, User, Users } from "lucide-react";
import { useSession } from "@/lib/session";
import { useToast } from "@/lib/toast";
import { Modal } from "@/lib/overlay";
import { Banner, Button, Card, Chip, CopyBlock, DataTable, Empty, Field, Notice, Section, Skeleton, Stat, StatRow, Status } from "@/lib/ui";
import { dateTime } from "@/lib/format";
import {
  DriveBreadcrumbs,
  driveHref,
  ErrorPage,
  forgeAct,
  ForgePage,
  ForgeStatus,
  pending,
  ToRepositories,
  useForgeRead,
  useRepositoryTrail,
  type AccessEntry,
  type PolicyObject,
  type Repository,
} from "./forge";
import { linkActions, linkState, type EffectiveAccess, type ShareLink } from "./share";

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
      above={<DriveBreadcrumbs repository={repository.data!} refName={refName} path={path} />}
      title={`Share ${path || "/"}`}
      repository={repository.data!}
      tab="files"
      sub="A folder's links are held in one workspace. Choose the workspace whose links you want to see or create."
    >
      {choices.length === 0 ? (
        <Empty icon={Layers} title="No workspace yet" action={<Button to="/workspaces">Create a workspace</Button>}>
          There is no workspace here yet. Create one before making a link.
        </Empty>
      ) : (
        <div className="grid grid-cols-1 gap-3 sm:grid-cols-2 lg:grid-cols-3">
          {choices.map((w) => (
            <Card key={w.id} to={driveHref("/drive/share", id, refName, path, { workspace_id: w.id })} label="Workspace" title={w.name}>
              <span className="text-[12.5px] text-ink-2">Hold this folder's links here</span>
            </Card>
          ))}
        </div>
      )}
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
  const [delivery, setDelivery] = useState<Delivery | null>(null);
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
  const direct = a.items.filter((entry) => !entry.inherited).length;
  const activeLinks = a.links.filter((link) => linkState(link) === "active").length;
  const deliveries: { key: Delivery; icon: typeof Users; title: string; text: string; button: string }[] = [
    { key: "person", icon: User, title: "People", text: "A named person receives a live folder. Updates and moves continue to apply.", button: "Share with person" },
    {
      key: "organization",
      icon: Users,
      title: "Organization",
      text: "A named organization identity receives the same live folder. People appear only when their identity has effective access above.",
      button: "Share with organization",
    },
    { key: "link", icon: Link2, title: "Link", text: "Anyone holding the link can open this live folder until its expiry or revocation.", button: "Create link" },
  ];
  return (
    <ForgePage
      above={<DriveBreadcrumbs repository={a.repository} refName={refName} path={path} />}
      title={`Share ${path || "/"}`}
      repository={a.repository}
      tab="files"
      sub="Review who can access this folder, then deliver it as a live folder, a continuing link, or an independent copy."
      actions={
        <>
          <Button onClick={() => setDelivery("person")}>
            <User size={14} aria-hidden /> Share with person
          </Button>
          <Button variant="primary" onClick={() => setDelivery("link")}>
            <Link2 size={14} aria-hidden /> Create link
          </Button>
        </>
      }
    >
      {error && (
        <Banner tone="warn" title="Revoke refused">
          {error}
        </Banner>
      )}
      {created && (
        <Notice title="Copy this link now">
          <p className="m-0">It is shown once; lake keeps only its hash.</p>
          <CopyBlock value={new URL(created, window.location.origin).toString()} />
        </Notice>
      )}
      <StatRow cols={3}>
        <Stat label="With access" value={a.items.length} icon={Users} sub={`${direct} direct, ${a.items.length - direct} inherited`} />
        <Stat label="Active links" value={activeLinks} icon={Link2} sub={`${a.links.length} in workspace ${heldIn}`} />
        <Stat label="Expired or revoked" value={a.links.length - activeLinks} icon={Ban} sub="no longer open the folder" />
      </StatRow>
      <div className="space-y-6">
        <Section title="Who Can Access This Folder Now" sub="The live policy result: people and organization identities, their actual actions, and whether each access path is inherited.">
          <DataTable<AccessEntry>
            what="access"
            caption="Who can access this folder"
            rows={a.items}
            rowKey={(_, index) => String(index)}
            empty={<Empty compact icon={ShieldCheck} title="No grants" />}
            columns={[
              { key: "who", label: "Who", render: (entry) => <code className="break-all">{entry.principal_taap_id}</code> },
              {
                key: "what",
                label: "What they can do",
                render: (entry) => (
                  <span className="flex flex-wrap items-center gap-1.5">
                    <ForgeStatus value={entry.role} />
                    <span className="text-[12.5px] text-ink-2">{entry.actions.join(", ")}</span>
                  </span>
                ),
              },
              { key: "how", label: "How access arrives", render: (entry) => <Chip tone={entry.inherited ? "neutral" : "action"}>{entry.inherited ? "inherited" : "direct"}</Chip> },
            ]}
          />
        </Section>
        <Section title="Choose a Delivery">
          <div className="grid grid-cols-1 gap-3 sm:grid-cols-2 xl:grid-cols-4">
            {deliveries.map((d) => (
              <Card
                key={d.key}
                label={
                  <span className="inline-flex items-center gap-1.5">
                    <d.icon size={13} /> live
                  </span>
                }
                title={d.title}
                footer={
                  <Button variant={d.key === "link" ? "primary" : "secondary"} onClick={() => setDelivery(d.key)}>
                    {d.button}
                  </Button>
                }
              >
                <p className="m-0 text-[13px] text-ink-2">{d.text}</p>
              </Card>
            ))}
            <Card
              tone="quiet"
              label={
                <span className="inline-flex items-center gap-1.5">
                  <Download size={13} /> not live
                </span>
              }
              title="Independent Copy"
            >
              <p className="m-0 text-[13px] text-ink-2">Downloaded bytes stop being a live object. They cannot be updated or recalled by changing access here.</p>
            </Card>
          </div>
        </Section>
        <Section title="Folder Share Management" sub="A link is a continuing capability, not an attachment. Revoking it prevents its next open; copies already downloaded remain independent.">
          <Banner tone="quiet">
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
          </Banner>
          <DataTable<ShareLink>
            what="links"
            caption="Folder links"
            rows={a.links}
            rowKey={(link) => link.id}
            empty={
              <Empty compact icon={Link2} title="No links" action={<Button onClick={() => setDelivery("link")}>Create link</Button>}>
                A link lets anyone holding it open this folder.
              </Empty>
            }
            columns={[
              { key: "who", label: "Who can access", render: () => "Anyone with this link" },
              { key: "what", label: "What they can do", render: (link) => linkActions(link.role) },
              { key: "until", label: "Until when", sort: (link) => link.expires_at, render: (link) => dateTime(link.expires_at) },
              { key: "state", label: "State", render: (link) => <Status value={linkState(link)} /> },
              {
                key: "actions",
                label: <span className="sr-only">Actions</span>,
                render: (link) => (
                  <span className="flex flex-wrap items-center justify-end gap-2">
                    <Button variant="ghost" to={driveHref("/drive/share/link", id, refName, path, { workspace_id: workspaceId, link_id: link.id })}>
                      Details
                    </Button>
                    <Button loading={revoking === link.id} disabled={!!revoking} onClick={() => void revoke(link.id)}>
                      Revoke link
                    </Button>
                  </span>
                ),
              },
            ]}
          />
        </Section>
      </div>
      {(delivery === "person" || delivery === "organization") && (
        <DirectShare
          id={id}
          refName={refName}
          path={path}
          heading={delivery === "person" ? "Share with a Person" : "Share with an Organization"}
          text={deliveries.find((d) => d.key === delivery)!.text}
          kind={delivery === "person" ? "user" : "organization"}
          principalLabel={delivery === "person" ? "Person (email or handle)" : "Organization (handle)"}
          roleLabel={delivery === "person" ? "What they can do" : "What it can do"}
          button={delivery === "person" ? "Share with person" : "Share with organization"}
          onClose={() => setDelivery(null)}
        />
      )}
      {delivery === "link" && (
        <CreateLink
          id={id}
          refName={refName}
          path={path}
          workspaceId={workspaceId}
          onCreated={(url) => {
            setCreated(url);
            setDelivery(null);
          }}
          onClose={() => setDelivery(null)}
        />
      )}
    </ForgePage>
  );
}

type Delivery = "person" | "organization" | "link";

const ROLES = [
  ["read", "View — browse and download"],
  ["write", "Edit — upload, rename, move, and delete"],
  ["share", "Share — browse and create shares"],
] as const;

/** A person who can be named in a share: drive_share_people (lake#585). */
type Person = { taap_id: string; kind: "user" | "organization"; display_name: string; email: string; handle: string };

/** People and Organization: a live grant on the folder, in a dialog. The person is named by email
 * or handle, picked from the space's people (drive_share_people); lake resolves their identity
 * and writes and signs the grant's attestation itself (lake#585). */
function DirectShare({
  id,
  refName,
  path,
  kind,
  heading,
  text,
  principalLabel,
  roleLabel,
  button,
  onClose,
}: {
  id: string;
  refName: string;
  path: string;
  kind: Person["kind"];
  heading: string;
  text: string;
  principalLabel: string;
  roleLabel: string;
  button: string;
  onClose: () => void;
}) {
  const toast = useToast();
  const people = useForgeRead<{ people: Person[] }>("drive_share_people", { repository_id: id, ref: refName, path });
  const [recipient, setRecipient] = useState("");
  const [role, setRole] = useState("read");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const choices = (people.data?.people ?? []).filter((person) => person.kind === kind);
  const listId = `drive-share-people-${kind}`;
  return (
    <Modal
      title={heading}
      subtitle={text}
      onClose={onClose}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button type="submit" form="drive-share-direct" variant="primary" loading={busy} disabled={!recipient.trim()}>
            {button}
          </Button>
        </>
      }
    >
      <form
        id="drive-share-direct"
        className="stack"
        onSubmit={async (e) => {
          e.preventDefault();
          setBusy(true);
          setError(null);
          try {
            await forgeAct("share_drive_folder", { repository_id: id, ref: refName, path, recipient: recipient.trim(), role });
            toast("Folder shared");
            onClose();
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
        <Field label={principalLabel} required>
          <input
            value={recipient}
            onChange={(e) => setRecipient(e.target.value)}
            list={listId}
            placeholder={kind === "user" ? "ana@example.com or @ana" : "@studio"}
            autoComplete="off"
            autoFocus
          />
          <datalist id={listId}>
            {choices.map((person) => (
              <option key={person.taap_id} value={person.email || (person.handle ? `@${person.handle}` : person.taap_id)}>
                {[person.display_name, person.handle ? `@${person.handle}` : ""].filter(Boolean).join(" · ")}
              </option>
            ))}
          </datalist>
          <small className="muted">
            {people.isLoading
              ? "Loading the people of this space…"
              : choices.length
                ? `Choose one of the ${choices.length} ${kind === "user" ? "people" : "organizations"} of this space, or type an email or handle.`
                : "Type the email or handle of someone in this space."}
          </small>
        </Field>
        <Field label={roleLabel}>
          <select value={role} onChange={(e) => setRole(e.target.value)}>
            {ROLES.map(([value, label]) => (
              <option key={value} value={value}>
                {label}
              </option>
            ))}
          </select>
        </Field>
        <Banner tone="quiet">Lake records the share as a signed statement and keeps its digest on the grant as evidence.</Banner>
      </form>
    </Modal>
  );
}

/** Link (serve_drive_share_link_create): a bearer link held in this workspace, in a dialog. Its
 * URL is a one-time secret, shown once on the page. */
function CreateLink({
  id,
  refName,
  path,
  workspaceId,
  onCreated,
  onClose,
}: {
  id: string;
  refName: string;
  path: string;
  workspaceId: string;
  onCreated: (url: string) => void;
  onClose: () => void;
}) {
  const [role, setRole] = useState("read");
  const [expires, setExpires] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  return (
    <Modal
      title="Create a Link"
      subtitle="Anyone holding the link can open this live folder until its expiry or revocation."
      onClose={onClose}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button type="submit" form="drive-share-link" variant="primary" loading={busy}>
            Create link
          </Button>
        </>
      }
    >
      <form
        id="drive-share-link"
        className="stack"
        onSubmit={async (e) => {
          e.preventDefault();
          setBusy(true);
          setError(null);
          try {
            const link = await forgeAct<{ url: string }>("create_drive_share_link", { repository_id: id, workspace_id: workspaceId, ref: refName, path, role, expires_at: expires });
            onCreated(link.url);
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
        <Field label="What link holders can do">
          <select value={role} onChange={(e) => setRole(e.target.value)}>
            <option value="read">View — browse and download</option>
            <option value="write">Write scope — capability-aware clients only</option>
          </select>
        </Field>
        <Field label="Until when (UTC)" required>
          <input value={expires} onChange={(e) => setExpires(e.target.value)} placeholder="2030-01-01T00:00:00Z" autoFocus />
        </Field>
        <Field label="Passcode" help="Passcodes are not supported for drive-share links. No passcode is sent or stored.">
          <input disabled placeholder="Not available for drive links" />
        </Field>
      </form>
    </Modal>
  );
}
