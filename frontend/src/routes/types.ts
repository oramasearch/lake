/** The route registry's types. One row per page: the router, the rail, the breadcrumbs and the
 * command palette all read it, so a page exists in one place. Each page group owns
 * `pages/<group>/routes.ts`; `routes.tsx` joins them. */
import type { ComponentType, LazyExoticComponent } from "react";

/** Who may see a page in the rail: V visitor, A signed-in account holder, O the operator. */
export type Audience = "visitor" | "account" | "operator";

/** The rail's groups, in order. "top" items have no group label; "home" is a signed-in person's
 * Home group (Ask Orama, Dashboard). */
export type SectionKey = "top" | "home" | "data" | "design" | "train" | "ship" | "organization" | "administration" | "developer" | "account" | "none";

export type Screen = {
  /** The router pattern, without the `/w/:workspaceId` prefix. Keep lake's existing URL and query
   * parameters exactly: links to them are already handed out. */
  path: string;
  /** Title Case. The document title is "lake — <title>". */
  title: string;
  section: SectionKey;
  /** Shown in the rail, in registry order within its section; a string overrides the label. */
  nav?: boolean | string;
  /** The rail icon (components/Icon.tsx). */
  icon?: string;
  /** Who sees it in the rail and who may open it. Signed-out visitors opening an account page are
   * sent to /sign-in. Default: account and operator. */
  audience?: Audience[];
  /** A workspace page: served under `/w/:workspaceId<path>`, and an unprefixed URL is redirected to
   * the current workspace. */
  workspace?: boolean;
  /** Other paths whose pages mark this rail item current (lake's navigation_routes), e.g. the list
   * page owning its detail pages. Matched exactly or as a parent followed by "/". */
  owns?: string[];
  /** Rendered without the shell (sign-in pages, public share pages). */
  bare?: boolean;
  /** A billing page (lake#564): absent where the lake does not bill (`viewer.billing`, LAKE_BILLING
   * off, as on an enterprise install). */
  billing?: boolean;
  component: LazyExoticComponent<ComponentType<object>> | ComponentType<object>;
};
