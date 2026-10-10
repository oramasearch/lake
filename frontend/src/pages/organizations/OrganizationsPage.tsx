/** Organizations (`/organizations`): the organizations the viewer can see as cards, the invitations
 * addressed to them, and for a person the dialogs to create an organization or turn their personal
 * workspace into one (web_ui_organizations, serve_organizations_page and its three POST handlers). */
import { useState } from "react";
import { Link, useNavigate } from "react-router-dom";
import { Building2, Mail, Plus, Repeat2, ShieldCheck, Users } from "lucide-react";
import { act, ApiError, read, useList, type Row } from "@/lib/api";
import { plural } from "@/lib/format";
import { Modal } from "@/lib/overlay";
import { useSession } from "@/lib/session";
import { useToast } from "@/lib/toast";
import { Banner, Button, Card, DataTable, Empty, Field, FilterChips, Form, Page, SearchInput, Skeleton, ErrorBlock, Stat, StatRow, Toolbar, fieldError } from "@/lib/ui";
import { Dotted, HANDLE_PATTERN, PanelEmpty, RoleBadge, When, countBy, matches, useMetaTrail } from "./shared";

/** Members are counted per card only while the list is short: one read per organization. */
const COUNT_MEMBERS_UP_TO = 24;

export default function OrganizationsPage() {
  const { viewer, refresh } = useSession();
  // Person mode: a signed-in person, not the operator credential (which has no user).
  const person = !!viewer.user_id;
  const organizations = useList<Row>("list_organizations", { limit: 500 }, { pick: "items" });
  // The person-only reads degrade silently, as on the server.
  const memberships = useList<Row>(person ? "list_memberships" : null, { limit: 500 }, { pick: "items" });
  const invitations = useList<Row>(person ? "list_invitations" : null, { limit: 100 }, { pick: "items" });
  const personal = person ? viewer.spaces.find((space) => space.personal) : undefined;
  useMetaTrail(organizations.data ? `${organizations.data.length} organizations` : null, "Organizations");
  const [dialog, setDialog] = useState<"create" | "join" | "convert" | null>(null);
  const [search, setSearch] = useState("");
  const [role, setRole] = useState<string | null>(null);

  const roles = new Map<string, string>();
  for (const membership of memberships.data ?? []) {
    if (!membership.team_id) roles.set(membership.organization_id, membership.role);
  }
  const all = organizations.data ?? [];
  const shown = all.filter((o) => matches(search, o.display_name, o.id, o.name) && (!role || roles.get(o.id) === role));
  const roleOptions = countBy(all, (o) => roles.get(o.id));
  const pending = invitations.data ?? [];

  return (
    <Page
      eyebrow="Organization"
      title="Organizations"
      sub="An organization has its own workspaces, repositories and apps, and people in it as owners, admins or members."
      actions={
        person ? (
          <>
            {personal && (
              <Button onClick={() => setDialog("convert")}>
                <Repeat2 size={14} aria-hidden /> Convert your workspace
              </Button>
            )}
            <Button onClick={() => setDialog("join")}>
              <Mail size={14} aria-hidden /> Join{pending.length > 0 ? ` (${pending.length})` : ""}
            </Button>
            <Button variant="primary" onClick={() => setDialog("create")}>
              <Plus size={14} aria-hidden /> New organization
            </Button>
          </>
        ) : undefined
      }
    >
      {pending.length > 0 && (
        <Banner
          title={`${plural(pending.length, "invitation")} waiting for you`}
          action={
            <Button variant="primary" onClick={() => setDialog("join")}>
              Review
            </Button>
          }
        >
          {pending
            .slice(0, 3)
            .map((i) => i.organization_name || i.organization_id)
            .join(", ")}
          {pending.length > 3 ? ` and ${pending.length - 3} more` : ""} invited you to join.
        </Banner>
      )}
      {organizations.isLoading ? (
        <Skeleton lines={5} />
      ) : organizations.isError ? (
        <ErrorBlock error={organizations.error} retry={() => void organizations.refetch()} what="organizations" />
      ) : all.length === 0 ? (
        <Empty
          icon={Building2}
          title="No organizations yet"
          action={
            person ? (
              <Button variant="primary" onClick={() => setDialog("create")}>
                <Plus size={14} aria-hidden /> New organization
              </Button>
            ) : undefined
          }
        >
          {person ? "You are not in an organization yet. Create one, or ask a colleague to invite you by your email address." : "No organizations yet. They appear here when a person creates one."}
        </Empty>
      ) : (
        <>
          <StatRow cols={person ? 4 : 2}>
            <Stat label="Organizations" value={all.length} icon={Building2} sub={person ? "you belong to or can see" : "on this lake"} />
            {person && <Stat label="You own" value={[...roles.values()].filter((r) => r === "owner").length} icon={ShieldCheck} sub="as owner" />}
            {person && <Stat label="You administer" value={[...roles.values()].filter((r) => r === "admin").length} sub="as admin" />}
            <Stat label="Invitations" value={pending.length} icon={Mail} sub={pending.length ? "waiting for you" : "none waiting"} />
          </StatRow>
          <Toolbar>
            <SearchInput value={search} onChange={setSearch} placeholder="Search organizations…" />
            {roleOptions.length > 0 && <FilterChips options={roleOptions} value={role} onChange={setRole} allLabel="Any role" />}
          </Toolbar>
          {shown.length === 0 ? (
            <PanelEmpty title="No organization matches">Clear the search or the role filter to see more.</PanelEmpty>
          ) : (
            <div className="grid grid-cols-1 gap-3 md:grid-cols-2 xl:grid-cols-3">
              {shown.map((o) => (
                <OrganizationCard key={o.id} organization={o} role={roles.get(o.id)} countMembers={all.length <= COUNT_MEMBERS_UP_TO} />
              ))}
            </div>
          )}
        </>
      )}
      {dialog === "join" && <JoinDialog invitations={pending} loading={invitations.isLoading} onJoined={refresh} onClose={() => setDialog(null)} />}
      {dialog === "create" && <CreateOrganization onCreated={refresh} onClose={() => setDialog(null)} />}
      {dialog === "convert" && personal && <ConvertPersonalSpace customerId={personal.id} handle={personal.name} onConverted={refresh} onClose={() => setDialog(null)} />}
    </Page>
  );
}

function OrganizationCard({ organization: o, role, countMembers }: { organization: Row; role?: string; countMembers: boolean }) {
  const members = useList<Row>(countMembers ? "list_memberships" : null, { organization_id: o.id, limit: 500 }, { pick: "items" });
  const id = encodeURIComponent(o.id);
  const count = members.data?.length;
  return (
    <Card
      label="Organization"
      title={<Link to={`/organization?id=${id}`}>{o.display_name || o.id}</Link>}
      actions={role ? <RoleBadge role={role} /> : undefined}
      footer={
        <Dotted>
          {[
            <Link key="invite" to={`/organization?id=${id}&tab=invitations`}>
              invite
            </Link>,
            <Link key="teams" to={`/teams?organization_id=${id}`}>
              teams
            </Link>,
            <Link key="members" to={`/members?organization_id=${id}`}>
              members
            </Link>,
            <Link key="invitations" to={`/invitations?organization_id=${id}`}>
              invitations
            </Link>,
            <Link key="access" to={`/organization/access?id=${id}`}>
              effective access
            </Link>,
            <Link key="sso" to={`/sso?organization_id=${id}`}>
              SSO
            </Link>,
          ]}
        </Dotted>
      }
    >
      <div className="flex flex-wrap items-center gap-x-4 gap-y-1 text-[12.5px] text-ink-2">
        {countMembers && (
          <span className="inline-flex items-center gap-1">
            <Users size={13} className="text-ink-3" aria-hidden />
            {members.isLoading ? "…" : members.isError ? "members hidden" : count === 500 ? "500+ members" : plural(count ?? 0, "member")}
          </span>
        )}
        <span>
          Created <When value={o.created_at} />
        </span>
      </div>
    </Card>
  );
}

function JoinDialog({ invitations, loading, onJoined, onClose }: { invitations: Row[]; loading: boolean; onJoined: () => void; onClose: () => void }) {
  const navigate = useNavigate();
  const toast = useToast();
  const [joining, setJoining] = useState<string | null>(null);
  const join = async (invitation: Row) => {
    setJoining(invitation.id);
    try {
      const membership = await act<Row>("join_organization", { invitation_id: invitation.id });
      onJoined();
      onClose();
      navigate(`/organization?id=${encodeURIComponent(membership.organization_id)}`);
    } catch (error) {
      toast((error as Error).message, "bad");
    } finally {
      setJoining(null);
    }
  };
  return (
    <Modal title="Invitations for You" subtitle="Join an organization you were invited to." onClose={onClose} width={620}>
      {loading ? (
        <Skeleton lines={2} />
      ) : invitations.length === 0 ? (
        <PanelEmpty title="No invitations waiting">Ask a colleague to invite you by your email address or your handle.</PanelEmpty>
      ) : (
        <DataTable
          rows={invitations}
          rowKey={(i) => i.id}
          columns={[
            { key: "organization", label: "Organization", render: (i) => <span className="font-semibold text-ink">{i.organization_name || i.organization_id}</span> },
            { key: "role", label: "Role", render: (i) => <RoleBadge role={i.role} /> },
            { key: "expires", label: "Expires", render: (i) => <When value={i.expires_at} /> },
            {
              key: "join",
              label: <span className="sr-only">Actions</span>,
              render: (i) => (
                <Button variant="primary" loading={joining === i.id} onClick={() => void join(i)}>
                  Join
                </Button>
              ),
            },
          ]}
        />
      )}
    </Modal>
  );
}

function CreateOrganization({ onCreated, onClose }: { onCreated: () => void; onClose: () => void }) {
  const navigate = useNavigate();
  const [displayName, setDisplayName] = useState("");
  const [name, setName] = useState("");
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState(false);
  const submit = async () => {
    setBusy(true);
    setError(null);
    try {
      const input: Row = { display_name: displayName };
      if (name) input.name = name;
      const space = await act<Row>("create_organization_space", input);
      onCreated();
      onClose();
      navigate(`/organization?id=${encodeURIComponent(space.organization.id)}&tab=invitations`);
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  const nameError = fieldError(error, "name");
  const displayNameError = fieldError(error, "display_name");
  return (
    <Modal
      title="New Organization"
      subtitle="You become its owner. It gets its own workspaces, separate from yours; invite colleagues next."
      onClose={onClose}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button variant="primary" type="submit" form="create-organization" loading={busy}>
            Create organization
          </Button>
        </>
      }
    >
      <Form id="create-organization" onSubmit={submit} error={nameError || displayNameError ? null : error}>
        <Field label="Organization name" required error={displayNameError}>
          <input name="display_name" placeholder="Acme Research" value={displayName} onChange={(e) => setDisplayName(e.target.value)} autoFocus />
        </Field>
        <Field
          label="Handle (optional)"
          error={nameError}
          help={
            <>
              Names the organization's workspace, as in <code>handle/repository</code>: lowercase letters, numbers and inner hyphens. Leave it empty and lake generates one.
            </>
          }
        >
          <input name="name" pattern={HANDLE_PATTERN} placeholder="acme-research" value={name} onChange={(e) => setName(e.target.value)} />
        </Field>
      </Form>
    </Modal>
  );
}

function ConvertPersonalSpace({ customerId, handle, onConverted, onClose }: { customerId: string; handle: string; onConverted: () => void; onClose: () => void }) {
  const navigate = useNavigate();
  const [displayName, setDisplayName] = useState("");
  const [confirm, setConfirm] = useState("");
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState(false);
  const submit = async () => {
    setBusy(true);
    setError(null);
    try {
      // The typed handle is a guard of the page, not of the capability: compare it with the
      // workspace's current handle, as serve_organization_convert did, before changing anything.
      const customer = await read<Row>("get_customer", { customer_id: customerId });
      if (confirm !== customer.name) {
        throw new ApiError(
          400,
          "invalid_input",
          `type the handle ${customer.name} exactly to confirm turning this workspace into an organization; nothing was changed`,
          "/confirm",
        );
      }
      const space = await act<Row>("convert_personal_space", { customer_id: customerId, display_name: displayName });
      onConverted();
      onClose();
      navigate(`/organization?id=${encodeURIComponent(space.organization.id)}&tab=invitations`);
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  const confirmError = fieldError(error, "confirm");
  const displayNameError = fieldError(error, "display_name");
  return (
    <Modal
      title="Turn Your Workspace into an Organization"
      onClose={onClose}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button variant="danger" type="submit" form="convert-workspace" loading={busy}>
            Turn into an organization
          </Button>
        </>
      }
    >
      <p className="mt-0">
        Your workspace <code>{handle}</code> becomes the organization's. Its handle, repositories, workspaces and apps stay where they are, so links and clones keep working. You become
        the organization's owner and can invite colleagues. lake gives you a new, empty workspace with a generated handle.
      </p>
      <Banner tone="warn">This cannot be undone here.</Banner>
      <Form id="convert-workspace" onSubmit={submit} error={confirmError || displayNameError ? null : error}>
        <Field label="Organization name" required error={displayNameError}>
          <input name="display_name" value={displayName} onChange={(e) => setDisplayName(e.target.value)} />
        </Field>
        <Field
          label={
            <>
              Type <code>{handle}</code> to confirm
            </>
          }
          required
          error={confirmError}
        >
          <input name="confirm" autoComplete="off" value={confirm} onChange={(e) => setConfirm(e.target.value)} />
        </Field>
      </Form>
    </Modal>
  );
}
