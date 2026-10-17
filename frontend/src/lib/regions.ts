/** Regions (lake#757, docs/ui/regions.md): what `server_info` says about this Lake's region and the
 * deployment's other regions, and the words the header, the sign-in and sign-up pages and the
 * Workspaces page show. Each region is a separate Lake with its own accounts, so the other regions
 * are links to their hosts, never a switch inside this one. A Lake with no region (nonprod, local)
 * shows none of it. Pure: `npm test` covers it. */

export type Region = { name: string; label: string };
export type RegionHost = { name: string; label: string; host: string };
/** The part of `server_info` this reads. Older Lakes send neither field. */
export type ServerRegions = { region?: Region | null; regions?: RegionHost[] | null };

/** This Lake's region, or null when it has none. */
export function currentRegion(info: ServerRegions | undefined): Region | null {
  const region = info?.region;
  return region && typeof region.name === "string" && region.name ? { name: region.name, label: region.label || region.name } : null;
}

/** The deployment's other regions, in the operator's order, each with a host to go to. Empty when
 * this Lake has no region. */
export function otherRegions(info: ServerRegions | undefined): RegionHost[] {
  const own = currentRegion(info);
  if (!own) return [];
  return (info?.regions ?? []).filter((r) => r && r.name !== own.name && typeof r.host === "string" && r.host !== "");
}

/** The front page of another region's Lake. Its accounts are its own, so nothing of this page's
 * path is carried there. */
export function regionUrl(region: RegionHost): string {
  return `https://${region.host}/`;
}

/** The sentence on the sign-in or sign-up page, or null when this Lake has no region. */
export function accountRegionSentence(info: ServerRegions | undefined, mode: "sign-in" | "sign-up"): string | null {
  const own = currentRegion(info);
  if (!own) return null;
  return mode === "sign-up"
    ? `Your account is created in ${own.label}. Each region is a separate Lake with its own accounts, and your workspaces stay in ${own.label}.`
    : `You are signing in to Lake in ${own.label}. An account belongs to the region it was created in.`;
}

/** A workspace's region: the region of the Lake it was created on. Null when there is none. */
export function workspaceRegionLabel(info: ServerRegions | undefined): string | null {
  return currentRegion(info)?.label ?? null;
}
