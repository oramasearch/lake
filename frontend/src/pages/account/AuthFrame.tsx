/** The frame of a bare sign-in page: lake's brand (to Home) and the theme switch above the page,
 * with no rail, so a visitor signing in sees only the sign-in. */
import { type ReactNode, useEffect } from "react";
import { Link, useLocation } from "react-router-dom";
import { useTheme } from "@/components/Shell";
import { Icon } from "@/components/Icon";
import { useSession } from "@/lib/session";
import { isLeaving, leaveFor, returnTarget } from "./signIn";
import "@/styles/account.css";

export function AuthFrame({ children, title }: { children: ReactNode; title: string }) {
  const [theme, toggleTheme] = useTheme();
  useEffect(() => {
    document.title = `lake — ${title}`;
  }, [title]);
  return (
    <div className="auth-frame">
      <a className="skip-link" href="#main-content">
        Skip to content
      </a>
      <header className="auth-header">
        <Link className="brand" to="/">
          <span className="brand-mark">
            <Icon name="lake" />
          </span>
          <span>lake</span>
        </Link>
        <button
          type="button"
          className="theme-toggle"
          onClick={toggleTheme}
          aria-label={`Use ${theme === "dark" ? "light" : "dark"} theme`}
          aria-pressed={theme === "dark"}
        >
          <Icon name="theme" />
        </button>
      </header>
      <main id="main-content" tabIndex={-1}>
        {children}
      </main>
    </div>
  );
}

/** A signed-in viewer opening a sign-in page goes where the server sent them: the pending return,
 * else the page's default. Renders nothing while it leaves. */
export function useSignedInRedirect(fallback: (administrator: boolean, clerk: boolean) => string): boolean {
  const { viewer } = useSession();
  const location = useLocation();
  const signedIn = viewer.signed_in;
  const target = signedIn ? returnTarget(location.search, fallback(viewer.administrator, viewer.clerk.enabled)) : null;
  useEffect(() => {
    // A sign-in on this page already chose where to go; the refreshed viewer must not redirect.
    if (target && !isLeaving()) leaveFor(target, true);
  }, [target]);
  return signedIn;
}
