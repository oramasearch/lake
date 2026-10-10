/** Organizations (`/organizations`): the organizations the viewer can see, the invitations
 * addressed to them, and for a person the forms to create an organization or turn their personal
 * space into one (web_ui_organizations, serve_organizations_page and its three POST handlers). */
import { useState } from "react";
import { Link, useNavigate } from "react-router-dom";
import { act, ApiError, read, useList, type Row } from "@/lib/api";
import { useSession } from "@/lib/session";
import { useToast } from "@/lib/toast";
import { Button, DataTable, Field, Form, Page, fieldError, type Column } from "@/lib/ui";
import { Dotted, HANDLE_PATTERN, Panel, PanelEmpty, RoleBadge, When, useMetaTrail } from "./shared";

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

  const roles = new Map<string, string>();
  for (const membership of memberships.data ?? []) {
    if (!membership.team_id) roles.set(membership.organization_id, membership.role);
  }

  const columns: Column<Row>[] = [
    { key: "organization", label: "Organization", render: (o) => <Link to={`/organization?id=${encodeURIComponent(o.id)}`}>{o.display_name || o.id}</Link> },
    { key: "role", label: "Your role", render: (o) => (roles.has(o.id) ? <RoleBadge role={roles.get(o.id)} /> : "—") },
    {
      key: "administration",
      label: "Administration",
      render: (o) => {
        const id = encodeURIComponent(o.id);
        return (
          <Dotted>
            {[
              <Link key="settings" to={`/organization?id=${id}`}>settings</Link>,
              <Link key="invite" to={`/organization?id=${id}&tab=invitations`}>invite</Link>,
              <Link key="teams" to={`/teams?organization_id=${id}`}>teams</Link>,
              <Link key="members" to={`/members?organization_id=${id}`}>members</Link>,
              <Link key="invitations" to={`/invitations?organization_id=${id}`}>invitations</Link>,
              <Link key="access" to={`/organization/access?id=${id}`}>effective access</Link>,
            ]}
          </Dotted>
        );
      },
    },
    { key: "sso", label: "SSO", render: (o) => <Link to={`/sso?organization_id=${encodeURIComponent(o.id)}`}>SSO</Link> },
    { key: "created", label: "Created", render: (o) => <When value={o.created_at} />, sort: (o) => o.created_at },
  ];

  return (
    <Page eyebrow="identity" title="Organizations" sub="An organization has its own workspaces, repositories and apps, and people in it as owners, admins or members.">
      {(invitations.data?.length ?? 0) > 0 && <InvitationsForYou invitations={invitations.data ?? []} onJoined={refresh} />}
      <DataTable
        q={organizations}
        what="organizations"
        rows={organizations.data}
        columns={columns}
        rowKey={(o) => o.id}
        empty={
          <PanelEmpty>
            {person
              ? "You are not in an organization yet. Create one below, or ask a colleague to invite you by your email address."
              : "No organizations yet. They appear here when a person creates one."}
          </PanelEmpty>
        }
      />
      {person && <CreateOrganization onCreated={refresh} />}
      {person && personal && <ConvertPersonalSpace customerId={personal.id} handle={personal.name} onConverted={refresh} />}
    </Page>
  );
}

function InvitationsForYou({ invitations, onJoined }: { invitations: Row[]; onJoined: () => void }) {
  const navigate = useNavigate();
  const toast = useToast();
  const [joining, setJoining] = useState<string | null>(null);
  const join = async (invitation: Row) => {
    setJoining(invitation.id);
    try {
      const membership = await act<Row>("join_organization", { invitation_id: invitation.id });
      onJoined();
      navigate(`/organization?id=${encodeURIComponent(membership.organization_id)}`);
    } catch (error) {
      toast((error as Error).message, "bad");
    } finally {
      setJoining(null);
    }
  };
  return (
    <Panel title="Invitations for You">
      <DataTable
        rows={invitations}
        rowKey={(i) => i.id}
        columns={[
          { key: "organization", label: "Organization", render: (i) => i.organization_name || i.organization_id },
          { key: "role", label: "Role", render: (i) => <RoleBadge role={i.role} /> },
          { key: "expires", label: "Expires", render: (i) => <When value={i.expires_at} /> },
          {
            key: "join",
            label: "",
            render: (i) => (
              <Button variant="primary" loading={joining === i.id} onClick={() => void join(i)}>
                Join
              </Button>
            ),
          },
        ]}
      />
    </Panel>
  );
}

function CreateOrganization({ onCreated }: { onCreated: () => void }) {
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
    <Panel title="Create an Organization">
      <p>You become its owner. It gets its own workspaces, separate from yours; invite colleagues next.</p>
      <Form onSubmit={submit} error={nameError || displayNameError ? null : error}>
        <Field label="Organization name" required error={displayNameError}>
          <input name="display_name" placeholder="Acme Research" value={displayName} onChange={(e) => setDisplayName(e.target.value)} />
        </Field>
        <Field label="Handle (optional)" error={nameError}>
          <input name="name" pattern={HANDLE_PATTERN} placeholder="acme-research" value={name} onChange={(e) => setName(e.target.value)} />
        </Field>
        <p className="muted">
          The handle names the organization's space, as in <code>handle/repository</code>. Use lowercase letters, numbers, and inner hyphens; leave it empty and lake generates one.
        </p>
        <div>
          <Button variant="primary" type="submit" loading={busy}>
            Create organization
          </Button>
        </div>
      </Form>
    </Panel>
  );
}

function ConvertPersonalSpace({ customerId, handle, onConverted }: { customerId: string; handle: string; onConverted: () => void }) {
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
      // space's current handle, as serve_organization_convert did, before changing anything.
      const customer = await read<Row>("get_customer", { customer_id: customerId });
      if (confirm !== customer.name) {
        throw new ApiError(
          400,
          "invalid_input",
          `type the handle ${customer.name} exactly to confirm turning this space into an organization; nothing was changed`,
          "/confirm",
        );
      }
      const space = await act<Row>("convert_personal_space", { customer_id: customerId, display_name: displayName });
      onConverted();
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
    <Panel title="Turn Your Workspace into an Organization">
      <p>
        Your workspace <code>{handle}</code> becomes the organization's. Its handle, repositories, workspaces and apps stay where they are, so links and clones keep
        working. You become the organization's owner and can invite colleagues. lake gives you a new, empty workspace with a generated handle.
      </p>
      <p className="muted">This cannot be undone here.</p>
      <Form onSubmit={submit} error={confirmError || displayNameError ? null : error}>
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
        <div>
          <Button type="submit" loading={busy}>
            Turn into an organization
          </Button>
        </div>
      </Form>
    </Panel>
  );
}
