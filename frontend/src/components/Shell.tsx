/** The shell: one global navigation for every console page.
 *
 * Left rail: lake's groups in order (Design, Train, Ship, Organization or Administration,
 * Developer), with lake's icons; the current group is open and lists its pages, the others show
 * their name and open their first page. Collapsible to icons; below 1024px an off-canvas drawer.
 * Top bar: the breadcrumb, the space and workspace picker, Go to (⌘K or /), the theme switch, Ask
 * Orama and the account menu. G then a group's letter jumps to it. */
import { type ReactNode, Suspense, useEffect, useMemo, useRef, useState } from "react";
import { Link, NavLink, useLocation, useNavigate } from "react-router-dom";
import { useSession } from "@/lib/session";
import { useTrailState, type Crumb } from "@/lib/trail";
import { cx, Kbd, Skeleton } from "@/lib/ui";
import { isRepeat } from "@/lib/clicks";
import { useTheme } from "@/lib/theme";
import { currentNav, matchScreen, navFor, sectionLabel, workspacePath, isWorkspacePath, localPath, type NavItem, type SectionKey } from "@/routes";
import { Icon } from "./Icon";
import { CommandPalette } from "./CommandPalette";
import { ScreenBoundary } from "./ScreenBoundary";
import { WorkspacePicker } from "./WorkspacePicker";
import { AskPanel } from "./AskPanel";

/** G then this letter jumps to the group's first page. */
const JUMPS: Record<string, SectionKey> = { d: "design", t: "train", s: "ship", o: "organization", a: "administration" };


function useMediaQuery(query: string): boolean {
  const [matches, setMatches] = useState(() => matchMedia(query).matches);
  useEffect(() => {
    const list = matchMedia(query);
    const on = () => setMatches(list.matches);
    list.addEventListener("change", on);
    return () => list.removeEventListener("change", on);
  }, [query]);
  return matches;
}

/** Sign out: lake's POST /logout revokes the session and clears the HttpOnly cookie. A real form
 * submit, so the server's redirect (and, under Clerk, its sign-out page) follows. */
export function signOut() {
  const form = document.createElement("form");
  form.method = "post";
  form.action = "/logout";
  document.body.appendChild(form);
  form.submit();
}

function withWorkspace(item: NavItem, workspaceId: string | null): string {
  return isWorkspacePath(item.path) ? workspacePath(workspaceId, item.path) : item.path;
}

export function Shell({ children }: { children: ReactNode }) {
  const session = useSession();
  const { viewer, workspaceId } = session;
  const location = useLocation();
  const navigate = useNavigate();
  const items = useMemo(() => navFor(session), [session]);
  const current = currentNav(location.pathname, items);
  const screen = matchScreen(location.pathname);
  const wide = useMediaQuery("(min-width: 64rem)");
  const [collapsed, setCollapsed] = useState(() => {
    try {
      return localStorage.getItem("lake-rail") === "collapsed";
    } catch {
      return false;
    }
  });
  const [drawer, setDrawer] = useState(false);
  const [palette, setPalette] = useState(false);
  const [ask, setAsk] = useState(false);
  const askToggledAt = useRef(Number.NEGATIVE_INFINITY);
  const pendingG = useRef(0);
  const [theme, toggleTheme] = useTheme();
  const trailState = useTrailState();
  const signedInPerson = viewer.signed_in && !viewer.administrator;

  useEffect(() => {
    setDrawer(false);
  }, [location.pathname]);

  // The document title: "lake — <page>".
  useEffect(() => {
    const title = trailState.title ?? screen?.title ?? "lake";
    document.title = title === "lake" ? "lake" : `lake — ${title}`;
  }, [trailState.title, screen?.title]);

  // ⌘K or / opens Go to; G then a letter jumps to a group; never while typing.
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      const target = e.target as HTMLElement;
      const typing = target.closest("input, textarea, select, [contenteditable=true]");
      if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === "k") {
        e.preventDefault();
        setPalette(true);
        return;
      }
      if (typing || e.metaKey || e.ctrlKey || e.altKey) return;
      if (e.key === "/") {
        e.preventDefault();
        setPalette(true);
        return;
      }
      if (e.key === "Escape" && drawer) {
        setDrawer(false);
        return;
      }
      const now = Date.now();
      if (e.key.toLowerCase() === "g") {
        pendingG.current = now;
        return;
      }
      if (now - pendingG.current < 1200) {
        pendingG.current = 0;
        const section = JUMPS[e.key.toLowerCase()];
        const first = items.find((i) => i.section === section);
        if (first) navigate(withWorkspace(first, workspaceId));
        return;
      }
      pendingG.current = 0;
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [items, navigate, workspaceId, drawer]);

  const toggleRail = () => {
    setCollapsed((c) => {
      try {
        localStorage.setItem("lake-rail", c ? "expanded" : "collapsed");
      } catch {
        /* ignore */
      }
      return !c;
    });
  };

  // The default trail: group › item › page, unless the page set its own.
  const trail: Crumb[] = useMemo(() => {
    if (trailState.trail) return trailState.trail;
    const crumbs: Crumb[] = [];
    if (current && current.section !== "top") {
      const first = items.find((i) => i.section === current.section);
      crumbs.push({ label: sectionLabel(current.section), to: first && !first.external ? withWorkspace(first, workspaceId) : undefined });
    }
    if (current) crumbs.push({ label: current.label, to: withWorkspace(current, workspaceId) });
    if (screen && screen.path !== "*" && (!current || (screen.path !== current.path && screen.title !== current.label))) {
      if (screen.section === "account" && signedInPerson) crumbs.unshift({ label: "Account", to: "/settings" });
      crumbs.push({ label: screen.title });
    }
    return crumbs;
  }, [trailState.trail, current, screen, items, workspaceId, signedInPerson]);

  const groups = useMemo(() => {
    const out: { section: SectionKey; items: NavItem[] }[] = [];
    for (const item of items) {
      const last = out[out.length - 1];
      if (last && last.section === item.section) last.items.push(item);
      else out.push({ section: item.section, items: [item] });
    }
    return out;
  }, [items]);

  const rail = (
    <aside className="sidebar" aria-label="Navigation">
      <Link className="brand" to={workspaceId && viewer.signed_in ? `/w/${workspaceId}/design/projects` : "/"}>
        <span className="brand-mark">
          <Icon name="lake" />
        </span>
        <span className="brand-name">lake</span>
      </Link>
      <nav className="primary-nav" aria-label="Primary">
        {groups.map((group) => {
          const open = group.section === "top" || current?.section === group.section || collapsed;
          return (
            <div key={group.section} className="nav-group">
              {group.section !== "top" &&
                (open ? (
                  <p className="nav-label">{sectionLabel(group.section)}</p>
                ) : (
                  <NavLink className="nav-link nav-section" to={withWorkspace(group.items[0], workspaceId)} title={sectionLabel(group.section)}>
                    <Icon name={group.items[0].icon} />
                    <span className="nav-text">{sectionLabel(group.section)}</span>
                  </NavLink>
                ))}
              {open &&
                group.items.map((item) =>
                  item.external ? (
                    <a key={item.path} className="nav-link" href={item.path} title={item.label}>
                      <Icon name={item.icon} />
                      <span className="nav-text">{item.label}</span>
                    </a>
                  ) : (
                    <Link
                      key={item.section + item.path}
                      className="nav-link"
                      to={withWorkspace(item, workspaceId)}
                      aria-current={current === item ? "page" : undefined}
                      title={item.label}
                    >
                      <Icon name={item.icon} />
                      <span className="nav-text">{item.label}</span>
                    </Link>
                  ),
                )}
            </div>
          );
        })}
      </nav>
      <div className="sidebar-footer">
        <button type="button" className="rail-toggle button-ghost" onClick={toggleRail} aria-label={collapsed ? "Expand the navigation" : "Collapse the navigation"}>
          <span aria-hidden>{collapsed ? "»" : "«"}</span>
          <span className="sidebar-footer-text">{collapsed ? "" : "Collapse"}</span>
        </button>
        {viewer.version && <span className="sidebar-footer-text">lake {viewer.version}</span>}
      </div>
    </aside>
  );

  const toggleAsk = (e: React.MouseEvent) => {
    const now = performance.now();
    if (isRepeat(askToggledAt.current, now, e.detail)) return;
    askToggledAt.current = now;
    setAsk((a) => !a);
  };

  return (
    <div className="app-shell" data-rail={collapsed && wide ? "collapsed" : undefined} data-drawer={drawer ? "open" : undefined} data-ask={ask ? "open" : undefined}>
      <a className="skip-link" href="#main-content">
        Skip to content
      </a>
      {rail}
      {drawer && !wide && <div className="drawer-scrim" onClick={() => setDrawer(false)} aria-hidden />}
      <div className="app-frame">
        <header className="topbar">
          <div className="topbar-start">
            <button type="button" className="icon-button menu-button" onClick={() => setDrawer(true)} aria-label="Open the navigation">
              <span aria-hidden>☰</span>
            </button>
            <div className="topbar-context">
              {viewer.signed_in && viewer.spaces.length > 0 && <WorkspacePicker />}
              <Breadcrumb trail={trail} />
            </div>
          </div>
          <div className="topbar-actions">
            {viewer.signed_in && (
              <button type="button" className="topbar-search" onClick={() => setPalette(true)} aria-label="Go to (⌘K)">
                <span className="search-text">Go to…</span>
                <Kbd>⌘K</Kbd>
              </button>
            )}
            <button
              type="button"
              className="theme-toggle"
              onClick={toggleTheme}
              aria-label={`Use ${theme === "dark" ? "light" : "dark"} theme`}
              aria-pressed={theme === "dark"}
            >
              <Icon name="theme" />
            </button>
            <SessionControl onAsk={toggleAsk} askOpen={ask} />
          </div>
        </header>
        <main id="main-content" tabIndex={-1}>
          <ScreenBoundary>
            <Suspense fallback={<div className="page-container"><Skeleton /></div>}>{children}</Suspense>
          </ScreenBoundary>
        </main>
      </div>
      {ask && signedInPerson && <AskPanel onClose={() => setAsk(false)} from={localPath(location.pathname) + location.search} />}
      {palette && <CommandPalette onClose={() => setPalette(false)} />}
    </div>
  );
}

function Breadcrumb({ trail }: { trail: Crumb[] }) {
  if (!trail.length) return null;
  return (
    <nav className="breadcrumbs topbar-trail" aria-label="Breadcrumb">
      <ol>
        {trail.map((crumb, i) => {
          const last = i === trail.length - 1;
          return (
            <li key={i}>
              {last ? (
                <span aria-current="page" title={crumb.full}>
                  {crumb.label}
                </span>
              ) : crumb.to ? (
                <Link to={crumb.to} title={crumb.full}>
                  {crumb.label}
                </Link>
              ) : (
                <span title={crumb.full}>{crumb.label}</span>
              )}
            </li>
          );
        })}
      </ol>
    </nav>
  );
}

/** The right side of the top bar for each kind of viewer (web_ui.cpp session_control). */
function SessionControl({ onAsk, askOpen }: { onAsk: (e: React.MouseEvent) => void; askOpen: boolean }) {
  const { viewer } = useSession();
  const [menu, setMenu] = useState(false);
  const box = useRef<HTMLDivElement>(null);
  const location = useLocation();
  useEffect(() => {
    setMenu(false);
  }, [location.pathname]);
  useEffect(() => {
    if (!menu) return;
    const close = (e: MouseEvent) => {
      if (!box.current?.contains(e.target as Node)) setMenu(false);
    };
    const key = (e: KeyboardEvent) => {
      if (e.key === "Escape") setMenu(false);
    };
    document.addEventListener("mousedown", close);
    document.addEventListener("keydown", key);
    return () => {
      document.removeEventListener("mousedown", close);
      document.removeEventListener("keydown", key);
    };
  }, [menu]);

  if (!viewer.signed_in) {
    return (
      <span className="session">
        <Link to="/sign-in">Sign in</Link>
        {!viewer.clerk.enabled && viewer.password_sign_up && <Link to="/sign-up">Create account</Link>}
      </span>
    );
  }
  if (viewer.administrator) {
    return (
      <span className="session">
        <button type="button" onClick={signOut}>
          Sign out
        </button>
      </span>
    );
  }
  return (
    <>
      <button type="button" className={cx("button ask-orama")} aria-pressed={askOpen} onClick={onAsk}>
        <Icon name="agent" />
        Ask Orama
      </button>
      <div className="account-menu" ref={box}>
        <button type="button" className="account-summary" aria-haspopup="menu" aria-expanded={menu} onClick={() => setMenu((m) => !m)}>
          <Icon name="identity" />
          <span>{viewer.display_name || "Account"}</span>
        </button>
        {menu && (
          <div className="account-menu-panel" role="menu">
            <Link role="menuitem" to="/settings">
              Settings
            </Link>
            <Link role="menuitem" to="/sessions">
              Sessions
            </Link>
            {!viewer.clerk.enabled && (
              <Link role="menuitem" to="/passkeys">
                Passkeys
              </Link>
            )}
            <Link role="menuitem" to="/tokens">
              Tokens
            </Link>
            <form
              onSubmit={(e) => {
                e.preventDefault();
                signOut();
              }}
            >
              <button type="submit" role="menuitem">
                Sign out
              </button>
            </form>
          </div>
        )}
      </div>
    </>
  );
}
