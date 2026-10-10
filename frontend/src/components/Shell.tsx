/** The shell, in rev-os's layout on lake's colours: a 2px brand strip; a left rail of lake's groups
 * (Design, Train, Ship, Organization or Administration) with the current group open and its pages
 * listed beneath on a hairline, Docs and Developer at its foot, then the signed-in person and the
 * collapse control; a slim top bar with the breadcrumb, the space and workspace picker, search
 * (⌘K or /), the theme switch and Ask Orama. Collapsible to icons; below 1024px the rail is an
 * off-canvas drawer. G then a group's letter jumps to it.
 *
 * On a phone (docs/ui/mobile-web.md) the top bar is compact: the breadcrumb shows its last level,
 * and search, the workspace pill and Ask Orama are icons; Ask Orama is a full-screen conversation
 * that closes when a link in it navigates, so the page shows. */
import { type ReactNode, Suspense, lazy, useEffect, useMemo, useRef, useState } from "react";
import { Link, useLocation, useNavigate } from "react-router-dom";
import {
  BookMarked,
  BookOpen,
  Building2,
  ChevronsLeft,
  ChevronsRight,
  Code2,
  Compass,
  Database,
  FlaskConical,
  Home,
  type LucideIcon,
  Menu,
  Moon,
  Rocket,
  Search,
  Share2,
  ShieldCheck,
  Sparkles,
  Sun,
  X,
} from "lucide-react";
import { useSession } from "@/lib/session";
import { useTrailState, type Crumb } from "@/lib/trail";
import { cx, Kbd, Skeleton } from "@/lib/ui";
import { isRepeat } from "@/lib/clicks";
import { useTheme } from "@/lib/theme";
import { currentNav, matchScreen, navFor, sectionLabel, workspacePath, isWorkspacePath, localPath, type NavItem, type SectionKey } from "@/routes";
import { CommandPalette } from "./CommandPalette";
import { ScreenBoundary } from "./ScreenBoundary";
import { WorkspacePicker } from "./WorkspacePicker";
import { LakeMark } from "./LakeMark";
import { resizingAsk, useAskWidth } from "@/lib/askWidth";
import { onOpenAsk, type AskRequest } from "@/lib/ask";
import { PHONE_QUERY } from "@/lib/phoneSheet";

// Ask Orama loads the first time it is opened, so the shell carries no Markdown or conversation code.
const AskPanel = lazy(() => import("./AskPanel").then((m) => ({ default: m.AskPanel })));

/** G then this letter jumps to the group's first page (R for Data: its first page is Repositories). */
const JUMPS: Record<string, SectionKey> = { r: "data", d: "design", t: "train", s: "ship", o: "organization", a: "administration" };

/** Each group's icon, and the icons of the ungrouped items at the top. */
const SECTION_ICONS: Partial<Record<SectionKey, LucideIcon>> = {
  data: Database,
  design: Compass,
  train: FlaskConical,
  ship: Rocket,
  organization: Building2,
  administration: ShieldCheck,
  developer: Code2,
};
const TOP_ICONS: Record<string, LucideIcon> = {
  "/": Home,
  "/repositories": BookMarked,
  "/shared-with-me": Share2,
  "/search": Search,
};

function useViewportWidth(): number {
  const [width, setWidth] = useState(() => window.innerWidth);
  useEffect(() => {
    const on = () => setWidth(window.innerWidth);
    window.addEventListener("resize", on);
    return () => window.removeEventListener("resize", on);
  }, []);
  return width;
}

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

function initials(name: string | undefined): string {
  const words = (name ?? "").trim().split(/[\s@._-]+/).filter(Boolean);
  return ((words[0]?.[0] ?? "") + (words[1]?.[0] ?? "")).toUpperCase() || "?";
}

type Group = { section: SectionKey; items: NavItem[] };

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
  const [askRequest, setAskRequest] = useState<AskRequest | null>(null);
  const askToggledAt = useRef(Number.NEGATIVE_INFINITY);
  const pendingG = useRef(0);
  const [theme, toggleTheme] = useTheme();
  const trailState = useTrailState();
  const signedInPerson = viewer.signed_in && !viewer.administrator;
  const rail = collapsed && wide;
  const [askWidth, setAskWidth] = useAskWidth();
  const viewport = useViewportWidth();
  // Ask Orama pushes the page aside only while the page keeps room to read; narrower, it lies over
  // the page, which keeps its own layout (rev-os's assistant rule).
  const askPushes = ask && viewport >= 1280 && viewport - (rail ? 60 : 232) - askWidth >= 900;

  useEffect(() => {
    setDrawer(false);
    // On a phone Ask Orama covers the page: a link in it that navigates closes it, so the page shows.
    if (matchMedia(PHONE_QUERY).matches) setAsk(false);
  }, [location.pathname]);

  // A page opens Ask Orama about something (the transcript page's "Ask Orama about this").
  useEffect(() => {
    return onOpenAsk((request) => {
      setAskRequest(request);
      setAsk(true);
    });
  }, []);

  // The document title: "lake — <page>".
  useEffect(() => {
    const title = trailState.title ?? screen?.title ?? "lake";
    document.title = title === "lake" ? "lake" : `lake — ${title}`;
  }, [trailState.title, screen?.title]);

  // ⌘K or / opens search; G then a letter jumps to a group; never while typing.
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      const target = e.target as HTMLElement;
      const typing = target.closest("input, textarea, select, [contenteditable=true]");
      if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === "k") {
        e.preventDefault();
        setPalette(true);
        return;
      }
      if (typing || e.metaKey || e.ctrlKey || e.altKey) {
        pendingG.current = 0;
        return;
      }
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
    const out: Group[] = [];
    for (const item of items) {
      const last = out[out.length - 1];
      if (last && last.section === item.section) last.items.push(item);
      else out.push({ section: item.section, items: [item] });
    }
    return out;
  }, [items]);
  const main = groups.filter((g) => g.section !== "developer");
  const developer = groups.find((g) => g.section === "developer");
  const docs = developer?.items.find((i) => i.path === "/docs");
  const tools: Group | undefined = developer ? { section: "developer", items: developer.items.filter((i) => i.path !== "/docs") } : undefined;

  const toggleAsk = (e: React.MouseEvent) => {
    const now = performance.now();
    if (isRepeat(askToggledAt.current, now, e.detail)) return;
    askToggledAt.current = now;
    setAsk((a) => !a);
  };

  const renderGroup = (group: Group, small = false) => {
    if (group.section === "top") {
      return group.items.map((item) => (
        <RailLink
          key={item.path}
          to={withWorkspace(item, workspaceId)}
          icon={TOP_ICONS[item.path] ?? BookMarked}
          label={item.label}
          active={current === item}
          rail={rail}
          small={small}
        />
      ));
    }
    if (!group.items.length) return null;
    // Docs sits with the developer group in the rail's data but has its own line at the foot.
    const active = !!current && current.section === group.section && current.path !== "/docs";
    const first = group.items[0];
    return (
      <div key={group.section}>
        <RailLink
          to={first.external ? first.path : withWorkspace(first, workspaceId)}
          external={first.external}
          icon={SECTION_ICONS[group.section] ?? Compass}
          label={sectionLabel(group.section)}
          active={active}
          rail={rail}
          small={small}
        />
        {active && !rail && group.items.length > 1 && (
          <div className="mt-0.5 mb-1.5 ml-[30px] border-l border-hairline">
            {group.items.map((item) =>
              item.external ? (
                <a key={item.path} href={item.path} className="-ml-px block border-l border-transparent py-[3px] pr-2 pl-3 text-[12.5px] text-ink-3 no-underline hover:text-ink">
                  {item.label}
                </a>
              ) : (
                <Link
                  key={item.section + item.path}
                  to={withWorkspace(item, workspaceId)}
                  aria-current={current === item ? "page" : undefined}
                  className={cx(
                    "-ml-px block border-l py-[3px] pr-2 pl-3 text-[12.5px] no-underline transition-colors",
                    current === item ? "border-action font-semibold text-action" : "border-transparent text-ink-3 hover:text-ink",
                  )}
                >
                  {item.label}
                </Link>
              ),
            )}
          </div>
        )}
      </div>
    );
  };

  return (
    <div className="min-h-screen bg-canvas" data-shell data-ask={ask ? "open" : undefined}>
      <div className="brand-strip fixed inset-x-0 top-0 z-40" />
      <a className="skip-link" href="#main-content">
        Skip to content
      </a>
      <div className={cx("min-h-screen transition-[grid-template-columns] duration-200 lg:grid", rail ? "lg:grid-cols-[60px_minmax(0,1fr)]" : "lg:grid-cols-[232px_minmax(0,1fr)]")}>
        {drawer && !wide && <div className="fixed inset-0 z-40 bg-black/40 lg:hidden" onClick={() => setDrawer(false)} aria-hidden />}
        <nav
          aria-label="Navigation"
          className={cx(
            "flex flex-col border-r border-hairline bg-surface pt-[max(2px,env(safe-area-inset-top))] pb-[env(safe-area-inset-bottom)] lg:pt-[2px] lg:pb-0",
            "fixed inset-y-0 left-0 z-50 w-[280px] max-w-[85vw] transition-transform duration-200",
            "lg:sticky lg:top-0 lg:z-auto lg:h-screen lg:w-auto lg:max-w-none lg:translate-x-0",
            drawer ? "translate-x-0 shadow-dialog" : "-translate-x-full",
          )}
        >
          <button type="button" onClick={() => setDrawer(false)} className="hit absolute top-[max(0.75rem,env(safe-area-inset-top))] right-3 z-10 rounded p-1.5 text-ink-3 hover:bg-raised hover:text-ink lg:hidden" aria-label="Close the navigation">
            <X size={17} />
          </button>
          <Link
            to={workspaceId && viewer.signed_in ? `/w/${workspaceId}/design/projects` : "/"}
            className={cx("flex items-center gap-2.5 border-b border-hairline py-3 no-underline", rail ? "justify-center px-0" : "px-4")}
          >
            <LakeMark />
            {!rail && (
              <div className="min-w-0 leading-tight">
                <div className="font-mono text-[9.5px] tracking-[0.2em] text-ink-3 uppercase">Orama</div>
                <div className="text-[15px] font-semibold tracking-[-0.01em] text-ink">lake</div>
              </div>
            )}
          </Link>
          <div className="flex-1 overflow-y-auto py-2" aria-label="Primary">
            {main.map((group) => renderGroup(group))}
          </div>
          <div className="border-t border-hairline py-1.5">
            {docs && <RailLink to="/docs" icon={BookOpen} label="Docs" active={current === docs} rail={rail} small />}
            {tools && renderGroup(tools, true)}
          </div>
          <RailPerson rail={rail} onToggle={toggleRail} />
        </nav>

        <div
          className={cx("min-w-0", !resizingAsk() && "transition-[margin] duration-200")}
          style={askPushes ? { marginRight: askWidth } : undefined}
        >
          <header className="shell-bar sticky top-[2px] z-30 flex h-12 min-w-0 items-center gap-1 border-b border-hairline bg-canvas/90 px-2 backdrop-blur sm:gap-3 sm:px-6">
            <button type="button" onClick={() => setDrawer(true)} className="hit -ml-1 inline-flex h-8 w-8 items-center justify-center rounded-[5px] text-ink-2 hover:bg-raised hover:text-ink lg:hidden" aria-label="Open the navigation">
              <Menu size={18} />
            </button>
            <Breadcrumb trail={trail} />
            <div className="flex-1" />
            {viewer.signed_in && viewer.spaces.length > 0 && <WorkspacePicker />}
            {viewer.signed_in && (
              <>
                <button
                  type="button"
                  onClick={() => setPalette(true)}
                  className="hidden h-8 w-[220px] items-center gap-2 rounded-[6px] border border-rule bg-surface px-2.5 text-[12.5px] text-ink-3 transition-colors hover:border-action md:flex"
                >
                  <Search size={14} />
                  <span className="flex-1 text-left">Search</span>
                  <Kbd>⌘K</Kbd>
                </button>
                <button type="button" onClick={() => setPalette(true)} className="hit inline-flex h-8 w-8 items-center justify-center rounded-[5px] text-ink-2 hover:bg-raised hover:text-ink md:hidden" aria-label="Search">
                  <Search size={16} />
                </button>
              </>
            )}
            <button
              type="button"
              onClick={toggleTheme}
              className="hit inline-flex h-8 w-8 items-center justify-center rounded-[5px] text-ink-2 hover:bg-raised hover:text-ink"
              aria-label={`Use ${theme === "dark" ? "light" : "dark"} theme`}
              aria-pressed={theme === "dark"}
              title="Theme"
            >
              {theme === "dark" ? <Sun size={16} /> : <Moon size={16} />}
            </button>
            <SessionActions onAsk={toggleAsk} askOpen={ask} />
          </header>
          <main id="main-content" tabIndex={-1}>
            <ScreenBoundary>
              <Suspense fallback={<div className="page-container"><Skeleton /></div>}>{children}</Suspense>
            </ScreenBoundary>
          </main>
        </div>
      </div>
      {ask && signedInPerson && (
        <Suspense fallback={null}>
          <AskPanel
            onClose={() => {
              setAsk(false);
              setAskRequest(null);
            }}
            request={askRequest}
            onRequestDone={() => setAskRequest(null)}
            from={localPath(location.pathname) + location.search}
            width={askWidth}
            onResize={setAskWidth}
            overlay={!askPushes}
          />
        </Suspense>
      )}
      {palette && <CommandPalette onClose={() => setPalette(false)} />}
    </div>
  );
}

function RailLink({
  to,
  icon: IconC,
  label,
  active,
  rail,
  small,
  external,
}: {
  to: string;
  icon: LucideIcon;
  label: string;
  active: boolean;
  rail: boolean;
  small?: boolean;
  external?: boolean;
}) {
  const className = cx(
    "relative mx-2 flex items-center gap-2.5 rounded-[5px] no-underline transition-colors",
    small ? "py-[5px] text-[13px]" : "py-[6px] text-[13.5px]",
    rail ? "justify-center px-0" : "px-2.5",
    active ? "bg-raised font-semibold text-action" : small ? "text-ink-3 hover:bg-raised/70 hover:text-ink" : "text-ink-2 hover:bg-raised/70 hover:text-ink",
  );
  const body = (
    <>
      {active && <span className="absolute top-1.5 bottom-1.5 -left-2 w-[2px] rounded-r bg-action" />}
      <IconC size={small ? 15 : 16} strokeWidth={active ? 2.3 : 1.9} />
      {!rail && <span className="truncate">{label}</span>}
    </>
  );
  return external ? (
    <a href={to} className={className} title={rail ? label : undefined}>
      {body}
    </a>
  ) : (
    <Link to={to} className={className} title={rail ? label : undefined} aria-current={active ? "page" : undefined}>
      {body}
    </Link>
  );
}

/** The foot of the rail: who is signed in (their account menu) and the collapse control. */
function RailPerson({ rail, onToggle }: { rail: boolean; onToggle: () => void }) {
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
  const name = viewer.administrator ? "Lake operator" : viewer.display_name || viewer.email || "Account";
  const role = viewer.administrator ? "Operator" : viewer.signed_in ? "Account" : "Visitor";
  return (
    <div ref={box} className={cx("relative flex items-center border-t border-hairline py-2", rail ? "flex-col gap-2 px-0" : "gap-2 px-3")}>
      {viewer.signed_in ? (
        <button
          type="button"
          onClick={() => setMenu((m) => !m)}
          aria-haspopup="menu"
          aria-expanded={menu}
          className={cx("flex min-w-0 items-center gap-2 rounded-[5px] p-1 text-left hover:bg-raised", !rail && "flex-1")}
          title={rail ? name : undefined}
        >
          <span className="inline-flex h-[26px] w-[26px] shrink-0 items-center justify-center rounded-full bg-action text-[10.5px] font-semibold text-on-action">
            {initials(name)}
          </span>
          {!rail && (
            <span className="min-w-0 leading-tight">
              <span className="block truncate text-[12.5px] font-medium text-ink">{name}</span>
              <span className="block truncate font-mono text-[10px] tracking-[0.06em] text-ink-3 uppercase">{role}</span>
            </span>
          )}
        </button>
      ) : (
        !rail && <span className="flex-1 font-mono text-[10px] tracking-[0.06em] text-ink-3 uppercase">{viewer.version ? `lake ${viewer.version}` : "lake"}</span>
      )}
      <button
        type="button"
        onClick={onToggle}
        className="hidden rounded p-1 text-ink-3 hover:bg-raised hover:text-ink lg:block"
        title={rail ? "Expand" : "Collapse"}
        aria-label={rail ? "Expand the navigation" : "Collapse the navigation"}
      >
        {rail ? <ChevronsRight size={15} /> : <ChevronsLeft size={15} />}
      </button>
      {menu && (
        <div role="menu" className="absolute bottom-full left-2 z-50 mb-1 w-[200px] overflow-hidden rounded-[6px] border border-hairline bg-overlay py-1 shadow-popover">
          {!viewer.administrator && (
            <>
              <MenuLink to="/settings">Settings</MenuLink>
              <MenuLink to="/sessions">Sessions</MenuLink>
              {!viewer.clerk.enabled && <MenuLink to="/passkeys">Passkeys</MenuLink>}
              <MenuLink to="/tokens">Tokens</MenuLink>
              <div className="my-1 border-t border-hairline" />
            </>
          )}
          <button type="button" role="menuitem" onClick={signOut} className="block w-full px-3 py-1.5 text-left text-[13px] text-ink hover:bg-raised">
            Sign out
          </button>
        </div>
      )}
    </div>
  );
}

function MenuLink({ to, children }: { to: string; children: ReactNode }) {
  return (
    <Link role="menuitem" to={to} className="block px-3 py-1.5 text-[13px] text-ink no-underline hover:bg-raised">
      {children}
    </Link>
  );
}

function Breadcrumb({ trail }: { trail: Crumb[] }) {
  if (!trail.length) return null;
  return (
    <nav aria-label="Breadcrumb" className="flex min-w-0 items-center gap-1.5 text-[13px]">
      {trail.map((crumb, i) => {
        const last = i === trail.length - 1;
        return (
          <span key={i} className={cx("min-w-0 items-center gap-1.5", last ? "flex" : "hidden sm:flex")}>
            {i > 0 && <span className="hidden text-rule sm:inline">›</span>}
            {last ? (
              <span aria-current="page" title={crumb.full} className="truncate font-semibold text-ink">
                {crumb.label}
              </span>
            ) : crumb.to ? (
              <Link to={crumb.to} title={crumb.full} className="truncate text-ink-3 no-underline hover:text-ink">
                {crumb.label}
              </Link>
            ) : (
              <span title={crumb.full} className="truncate text-ink-3">
                {crumb.label}
              </span>
            )}
          </span>
        );
      })}
    </nav>
  );
}

/** The top bar's right edge: Ask Orama for a signed-in person; Sign in (and Create account where
 * it is open) for a visitor. The account menu lives at the foot of the rail. */
function SessionActions({ onAsk, askOpen }: { onAsk: (e: React.MouseEvent) => void; askOpen: boolean }) {
  const { viewer } = useSession();
  if (!viewer.signed_in) {
    return (
      <>
        {!viewer.clerk.enabled && viewer.password_sign_up && (
          <Link to="/sign-up" className="hidden h-8 items-center rounded-[6px] px-2.5 text-[12.5px] font-medium text-ink-2 no-underline hover:bg-raised hover:text-ink sm:inline-flex">
            Create account
          </Link>
        )}
        <Link to="/sign-in" className="inline-flex h-8 items-center rounded-[6px] border border-action bg-action px-3 text-[12.5px] font-medium text-on-action no-underline hover:brightness-110">
          Sign in
        </Link>
      </>
    );
  }
  if (viewer.administrator) return null;
  return (
    <button
      type="button"
      onClick={onAsk}
      aria-pressed={askOpen}
      aria-label="Ask Orama"
      title="Ask Orama"
      className={cx(
        "hit inline-flex h-8 shrink-0 items-center gap-1.5 rounded-[6px] border px-2 text-[12.5px] font-medium transition-colors sm:px-2.5",
        askOpen ? "border-action bg-action text-on-action" : "border-rule bg-surface text-ink-2 hover:border-action hover:text-action",
      )}
    >
      <Sparkles size={14} />
      <span className="hidden sm:inline">Ask Orama</span>
    </button>
  );
}
