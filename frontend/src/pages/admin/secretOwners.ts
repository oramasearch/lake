/** Who owns a secret mount (docs/ui/secrets.md, docs/api/secrets.md): the account that owns the
 * workspace, or an organization inside that account's space. `create_secret_mount` takes no
 * `organization_id` for the account and the organization's id otherwise. Pure, so `npm test`
 * checks it. */

/** One choice of the New Mount modal's Owner field. `organizationId` is empty for the account. */
export type OwnerOption = { organizationId: string; label: string };

/** The space the workspace belongs to, as the session names it. */
export type OwnerSpace = { label: string; personal: boolean };

/** An organization the person administers. `customer_id` is the organization's own space when it
 * has one (`get_organization`), null when it was created inside another space, undefined when not
 * known yet. */
export type AdministeredOrganization = { id: string; display_name?: string; name?: string; customer_id?: string | null };

function organizationName(organization: { id: string; display_name?: string; name?: string } | undefined, id: string): string {
  return organization?.display_name || organization?.name || id;
}

/** The account's own name in the UI: "Your account" for the person's own space, else the space's
 * label. */
export function accountLabel(space: OwnerSpace | null | undefined): string {
  if (!space || space.personal) return "Your account";
  return space.label;
}

/** The Owner field's choices, the workspace's owner first (the default): its organization when the
 * workspace belongs to an organization's space, else the account. Then each organization the person
 * administers that can own a mount here: never one whose own space is a different space. */
export function ownerOptions(input: {
  space: OwnerSpace | null | undefined;
  workspaceCustomerId: string;
  /** The workspace's customer's organization: set when the space is an organization's. */
  ownerOrganizationId?: string | null;
  administered: AdministeredOrganization[];
}): OwnerOption[] {
  const owner = input.ownerOrganizationId || "";
  const options: OwnerOption[] = [];
  if (owner) {
    const organization = input.administered.find((o) => o.id === owner);
    options.push({ organizationId: owner, label: `${organization ? organizationName(organization, owner) : input.space?.label || owner} (workspace owner)` });
  } else {
    options.push({ organizationId: "", label: accountLabel(input.space) });
  }
  const seen = new Set([owner]);
  for (const organization of input.administered) {
    if (seen.has(organization.id)) continue;
    if (organization.customer_id && organization.customer_id !== input.workspaceCustomerId) continue;
    seen.add(organization.id);
    options.push({ organizationId: organization.id, label: organizationName(organization, organization.id) });
  }
  return options;
}

/** The organizations a person administers, from their own memberships: an `owner` or `admin`
 * membership of the organization itself (a team membership does not administer it). */
export function administeredOrganizationIds(memberships: { organization_id?: string; team_id?: string | null; role?: string }[]): Set<string> {
  const ids = new Set<string>();
  for (const membership of memberships) {
    if (membership.organization_id && !membership.team_id && (membership.role === "owner" || membership.role === "admin")) ids.add(membership.organization_id);
  }
  return ids;
}

/** A mount's owner as the Secret Mounts table shows it. */
export function mountOwnerLabel(
  mount: { owner_type?: string; organization_id?: string | null },
  organizations: Map<string, { id: string; display_name?: string; name?: string }>,
  space: OwnerSpace | null | undefined,
): string {
  if (mount.organization_id) return organizationName(organizations.get(mount.organization_id), mount.organization_id);
  return accountLabel(space);
}
