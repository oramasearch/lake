/** Organization settings (`/organization?id=&tab=`): overview, teams, members, invitations,
 * access, SSO, applications, usage and audit, each a tab in the query (web_ui_organization,
 * serve_organization_page and the team, invitation and SSO form handlers). */
import { useState } from "react";
import { Link, Navigate } from "react-router-dom";
import { act, ApiError, useList, useRead, type Row } from "@/lib/api";
import { bytes } from "@/lib/format";
import { useToast } from "@/lib/toast";
import { Button, DataTable, ErrorBlock, Field, Form, Page, Skeleton, Status, fieldError, type Column } from "@/lib/ui";
import { HANDLE_PATTERN, Panel, PanelEmpty, RoleBadge, When, useMetaTrail, useParam } from "./shared";

const TABS: [string, string][] = [
  ["overview", "Overview"],
  ["teams", "Teams"],
  ["members", "Members"],
  ["invitations", "Invitations"],
  ["access", "Access"],
  ["sso", "SSO"],
  ["applications", "Applications"],
  ["metering", "Usage"],
  ["audit", "Audit"],
];

/** An invitation's `expires_at` from the form's day count, as serve_organization_invitation
 * computed it: a whole number of days from 1 to 90, from now, in UTC to the second. */
export function invitationExpiry(days: string, now = Date.now()): string {
  const count = days === "" ? 7 : Number(days);
  if (!Number.isInteger(count) || count < 1 || count > 90) {
    throw new ApiError(400, "invalid_input", "an invitation lasts from 1 to 90 days", "/expires_in_days");
  }
  return new Date(now + count * 86_400_000).toISOString().replace(/\.\d{3}Z$/, "Z");
}

export default function OrganizationPage() {
  const id = useParam("id");
  const tab = useParam("tab") || "overview";
  const organization = useRead<Row>(id ? "get_organization" : null, { id });
  const teams = useList<Row>(id ? "list_teams" : null, { organization_id: id, limit: 500 }, { pick: "items" });
  const memberships = useList<Row>(id ? "list_memberships" : null, { organization_id: id, limit: 500 }, { pick: "items" });
  // Admin-only: a plain member sees its 403 in the invitations panel, not instead of the page.
  const invitations = useList<Row>(id && (tab === "overview" || tab === "invitations") ? "list_invitations" : null, { organization_id: id, limit: 500 }, { pick: "items" });
  useMetaTrail(organization.data?.display_name, "Organization Settings");

  if (!id) return <Navigate to="/organizations" replace />;
  const fatal = [organization, teams, memberships].find((q) => q.isError);
  if (fatal) {
    return (
      <Page eyebrow="organization settings" title="Organization Settings">
        <ErrorBlock error={fatal.error} retry={() => void fatal.refetch()} what="the organization" />
      </Page>
    );
  }
  if (!organization.data || !teams.data || !memberships.data) {
    return (
      <Page eyebrow="organization settings" title="Organization Settings">
        <Skeleton />
      </Page>
    );
  }
  const org = organization.data;
  const base = `/organization?id=${encodeURIComponent(id)}&tab=`;
  return (
    <Page eyebrow="organization settings" title={org.display_name} sub="Organization administration, identity, access, and evidence.">
      <nav className="tabs" aria-label="Organization settings">
        {TABS.map(([name, label]) => (
          <Link key={name} to={base + name} aria-current={tab === name ? "page" : undefined}>
            {label}
          </Link>
        ))}
      </nav>
      {tab === "overview" && (
        <Panel>
          <dl>
            <dt>Identity</dt>
            <dd>
              <code>{org.taap_id ?? "—"}</code>
            </dd>
            <dt>Teams</dt>
            <dd>{teams.data.length}</dd>
            <dt>Members</dt>
            <dd>{memberships.data.length}</dd>
            <dt>Pending invitations</dt>
            <dd>{invitations.isLoading ? "…" : invitations.data ? invitations.data.length : "—"}</dd>
          </dl>
        </Panel>
      )}
      {tab === "teams" && <TeamsTab organizationId={id} teams={teams.data} />}
      {tab === "members" && <MembersTab base={base} memberships={memberships.data} />}
      {tab === "invitations" && <InvitationsTab organizationId={id} invitations={invitations} />}
      {tab === "access" && <AccessTab organizationId={id} />}
      {tab === "sso" && <SsoTab organizationId={id} />}
      {tab === "applications" && <ApplicationsTab organizationId={id} />}
      {tab === "metering" && <UsageTab organizationId={id} />}
      {tab === "audit" && <AuditTab organizationId={id} />}
    </Page>
  );
}

function TeamsTab({ organizationId, teams }: { organizationId: string; teams: Row[] }) {
  const toast = useToast();
  const [name, setName] = useState("");
  const [displayName, setDisplayName] = useState("");
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState(false);
  const submit = async () => {
    setBusy(true);
    setError(null);
    try {
      const input: Row = { organization_id: organizationId, name };
      if (displayName) input.display_name = displayName;
      await act("create_team", input);
      setName("");
      setDisplayName("");
      toast("Team created");
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  const nameError = fieldError(error, "name");
  return (
    <>
      <Panel title="Teams">
        {teams.map((team) => (
          <p key={team.id}>
            <Link to={`/team?id=${encodeURIComponent(team.id)}`}>{team.display_name || team.name}</Link>
          </p>
        ))}
        {teams.length === 0 && <p className="empty">No teams in this organization yet.</p>}
      </Panel>
      <Panel title="Create a Team">
        <Form onSubmit={submit} error={nameError ? null : error}>
          <Field label="Team name" required error={nameError}>
            <input name="name" pattern={HANDLE_PATTERN} placeholder="reviewers" value={name} onChange={(e) => setName(e.target.value)} />
          </Field>
          <Field label="Display name">
            <input name="display_name" value={displayName} onChange={(e) => setDisplayName(e.target.value)} />
          </Field>
          <div>
            <Button variant="primary" type="submit" loading={busy}>
              Create team
            </Button>
          </div>
        </Form>
      </Panel>
    </>
  );
}

function MembersTab({ base, memberships }: { base: string; memberships: Row[] }) {
  return (
    <Panel title="Members">
      {memberships.map((member) => (
        <p key={member.id}>
          <Link to={`/member?id=${encodeURIComponent(member.id)}`}>{member.principal_taap_id}</Link> <RoleBadge role={member.role} />
        </p>
      ))}
      {memberships.length === 0 ? (
        <p className="empty">
          No members yet. <Link to={base + "invitations"}>Invite people</Link> by email address or handle.
        </p>
      ) : (
        <p>
          <Link to={base + "invitations"}>Invite more people</Link>
        </p>
      )}
    </Panel>
  );
}

function InvitationsTab({ organizationId, invitations }: { organizationId: string; invitations: ReturnType<typeof useList<Row>> }) {
  const toast = useToast();
  const [invitee, setInvitee] = useState("");
  const [inviteeType, setInviteeType] = useState("email");
  const [role, setRole] = useState("member");
  const [days, setDays] = useState("7");
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState(false);
  const submit = async () => {
    setBusy(true);
    setError(null);
    try {
      const expiresAt = invitationExpiry(days);
      await act("create_invitation", { organization_id: organizationId, invitee_type: inviteeType, invitee, role, expires_at: expiresAt });
      setInvitee("");
      toast("Invitation sent");
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  const inviteeError = fieldError(error, "invitee");
  const daysError = fieldError(error, "expires_in_days") ?? fieldError(error, "expires_at");
  const columns: Column<Row>[] = [
    { key: "invitee", label: "Invitee", render: (i) => `${i.invitee_type}: ${i.invitee}` },
    { key: "role", label: "Role", render: (i) => <RoleBadge role={i.role} /> },
    { key: "status", label: "Status", render: (i) => <Status value={i.status} /> },
    { key: "expires", label: "Expires", render: (i) => <When value={i.expires_at} />, sort: (i) => i.expires_at },
  ];
  return (
    <>
      <Panel title="Invite a Member">
        <p>The person joins from their Organizations page once they sign in with that email address, or with the personal space that has that handle.</p>
        <Form onSubmit={submit} error={inviteeError || daysError ? null : error}>
          <Field label="Email or handle" required error={inviteeError}>
            <input name="invitee" value={invitee} onChange={(e) => setInvitee(e.target.value)} />
          </Field>
          <Field label="Type">
            <select name="invitee_type" value={inviteeType} onChange={(e) => setInviteeType(e.target.value)}>
              <option value="email">Email</option>
              <option value="handle">Handle</option>
            </select>
          </Field>
          <Field label="Role">
            <select name="role" value={role} onChange={(e) => setRole(e.target.value)}>
              <option value="member">Member: can read the organization</option>
              <option value="admin">Admin: can manage people, teams and access</option>
              <option value="owner">Owner: can manage; an organization always keeps at least one</option>
            </select>
          </Field>
          <Field label="Expires in" error={daysError}>
            <select name="expires_in_days" value={days} onChange={(e) => setDays(e.target.value)}>
              <option value="1">1 day</option>
              <option value="7">7 days</option>
              <option value="30">30 days</option>
            </select>
          </Field>
          <div>
            <Button variant="primary" type="submit" loading={busy}>
              Send invitation
            </Button>
          </div>
        </Form>
      </Panel>
      <DataTable
        q={invitations}
        what="invitations"
        rows={invitations.data}
        columns={columns}
        rowKey={(i) => i.id}
        empty={<PanelEmpty>No invitations yet. Invitations you send appear here until they are accepted or expire.</PanelEmpty>}
      />
    </>
  );
}

function AccessTab({ organizationId }: { organizationId: string }) {
  const access = useRead<Row>("entity_effective_access", { object_type: "organization", id: organizationId });
  const items: Row[] = access.data?.items ?? [];
  return (
    <Panel title="Effective Access">
      {access.isLoading ? (
        <Skeleton />
      ) : access.isError ? (
        <ErrorBlock error={access.error} retry={() => void access.refetch()} what="effective access" />
      ) : (
        <>
          {items.map((entry, index) => (
            <p key={index}>
              <code>{entry.principal_taap_id}</code> <RoleBadge role={entry.role} />
            </p>
          ))}
          {items.length === 0 && <p className="empty">No effective access entries.</p>}
        </>
      )}
    </Panel>
  );
}

function SsoTab({ organizationId }: { organizationId: string }) {
  const toast = useToast();
  const providers = useList<Row>("list_sso_providers", { organization_id: organizationId, limit: 100 }, { pick: "items" });
  const [form, setForm] = useState({ name: "", protocol: "oidc", issuer: "", scim_token: "" });
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState(false);
  const set = (key: keyof typeof form) => (e: { target: { value: string } }) => setForm((f) => ({ ...f, [key]: e.target.value }));
  const submit = async () => {
    setBusy(true);
    setError(null);
    try {
      await act("register_sso_provider", { organization_id: organizationId, ...form });
      setForm({ name: "", protocol: "oidc", issuer: "", scim_token: "" });
      toast("Provider saved");
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  const errors = { name: fieldError(error, "name"), issuer: fieldError(error, "issuer"), scim_token: fieldError(error, "scim_token") };
  return (
    <>
      <Panel title="Configure SSO Provider">
        <Form onSubmit={submit} error={errors.name || errors.issuer || errors.scim_token ? null : error}>
          <Field label="Name" required error={errors.name}>
            <input name="name" value={form.name} onChange={set("name")} />
          </Field>
          <Field label="Protocol">
            <select name="protocol" value={form.protocol} onChange={set("protocol")}>
              <option value="oidc">OpenID Connect</option>
              <option value="saml">SAML 2.0</option>
            </select>
          </Field>
          <Field label="Issuer" required error={errors.issuer}>
            <input name="issuer" value={form.issuer} onChange={set("issuer")} />
          </Field>
          <Field label="SCIM token" required error={errors.scim_token}>
            <input name="scim_token" autoComplete="off" value={form.scim_token} onChange={set("scim_token")} />
          </Field>
          <div>
            <Button variant="primary" type="submit" loading={busy}>
              Save provider
            </Button>
          </div>
        </Form>
      </Panel>
      {providers.isError ? (
        <ErrorBlock error={providers.error} retry={() => void providers.refetch()} what="SSO providers" />
      ) : (
        (providers.data ?? []).map((provider) => (
          <p key={provider.id} className="panel">
            <code>{provider.name}</code> {provider.protocol}
          </p>
        ))
      )}
    </>
  );
}

/** The organization's own space: no capability maps an organization to it, so find the customer
 * whose `organization_id` is this one, as the server page did. */
function useOrganizationSpace(organizationId: string) {
  const customers = useList<Row>("list_customers", { limit: 500 }, { pick: "items" });
  const space = customers.data?.find((customer) => customer.organization_id === organizationId);
  return { customers, space };
}

function ApplicationsTab({ organizationId }: { organizationId: string }) {
  const { customers, space } = useOrganizationSpace(organizationId);
  const applications = useList<Row>(space ? "list_applications" : null, { customer_id: space?.id, limit: 100 }, { pick: "items" });
  const failed = customers.isError ? customers : applications.isError ? applications : null;
  return (
    <Panel title="Applications">
      {customers.isLoading || applications.isLoading ? (
        <Skeleton />
      ) : failed ? (
        <ErrorBlock error={failed.error} retry={() => void failed.refetch()} what="applications" />
      ) : (
        <>
          {(applications.data ?? []).map((application) => (
            <p key={application.id}>
              <Link to={`/application?id=${encodeURIComponent(application.id)}`}>{application.name || application.id}</Link>
            </p>
          ))}
          {(applications.data ?? []).length === 0 && <p className="empty">No applications belong to this organization customer.</p>}
        </>
      )}
    </Panel>
  );
}

function UsageTab({ organizationId }: { organizationId: string }) {
  const { customers, space } = useOrganizationSpace(organizationId);
  const usage = useRead<Row>(space ? "metering_usage" : null, { customer_id: space?.id });
  const failed = customers.isError ? customers : usage.isError ? usage : null;
  let body;
  if (customers.isLoading || usage.isLoading) body = <Skeleton />;
  else if (failed) body = <ErrorBlock error={failed.error} retry={() => void failed.refetch()} what="usage" />;
  else if (!space) body = <p className="empty">No space is attached to this organization.</p>;
  else {
    const quota = Number(usage.data?.quota_bytes ?? 0);
    const total = usage.data?.summary?.storage?.total_bytes ?? 0;
    body = (
      <>
        <p>
          Stored: {bytes(total)}. Repository storage limit: {quota === 0 ? "no limit" : bytes(quota)}.
        </p>
        <p>
          <Link to={`/metering?customer_id=${encodeURIComponent(space.id)}`}>Open usage</Link>
        </p>
      </>
    );
  }
  return <Panel title="Usage">{body}</Panel>;
}

function AuditTab({ organizationId }: { organizationId: string }) {
  // Organization events are control-plane events: the central log alone (decision 52).
  const audit = useList<Row>("audit", { object: `organization/${organizationId}`, limit: 100, log: "central" }, { pick: "items" });
  return (
    <Panel title="Recent Audit Outcomes">
      {audit.isLoading ? (
        <Skeleton />
      ) : audit.isError ? (
        <ErrorBlock error={audit.error} retry={() => void audit.refetch()} what="audit events" />
      ) : (
        <>
          {(audit.data ?? []).map((event) => (
            <p key={event.id}>
              <code>{event.action}</code> <Status value={event.outcome} />
            </p>
          ))}
          {(audit.data ?? []).length === 0 && <p className="empty">No matching audit events.</p>}
        </>
      )}
    </Panel>
  );
}
