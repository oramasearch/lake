/** The frame of a bare sign-in page: lake's brand (to Home) and the theme switch above the page,
 * with no rail, so a visitor signing in sees only the sign-in. */
import { type ReactNode, useEffect } from "react";
import { Link, useLocation } from "react-router-dom";
import { useTheme } from "@/lib/theme";
import { Moon, Sun } from "lucide-react";
import { LakeMark } from "@/components/LakeMark";
import { useSession } from "@/lib/session";
import { isLeaving, leaveFor, returnTarget } from "./signIn";
import "@/styles/account.css";

export function AuthFrame({ children, title }: { children: ReactNode; title: string }) {
  const [theme, toggleTheme] = useTheme();
  useEffect(() => {
    document.title = `lake — ${title}`;
  }, [title]);
  return (
    <div className="auth-plane">
      <div className="brand-strip fixed inset-x-0 top-0 z-40" />
      <a className="skip-link" href="#main-content">
        Skip to content
      </a>
      <button
        type="button"
        className="auth-theme"
        onClick={toggleTheme}
        aria-label={`Use ${theme === "dark" ? "light" : "dark"} theme`}
        aria-pressed={theme === "dark"}
      >
        {theme === "dark" ? <Sun size={16} /> : <Moon size={16} />}
      </button>
      <div className="auth-layout">
        <section className="auth-brand" aria-label="lake">
          <Link to="/" className="auth-brand-link">
            <LakeMark size={30} />
            <span className="auth-kicker">Orama</span>
          </Link>
          <p className="auth-name">lake</p>
          <p className="auth-tagline">Design, train and ship AI on your own data, in your own workspace.</p>
          <ul className="auth-stages">
            <li>Design</li>
            <li>Train</li>
            <li>Ship</li>
          </ul>
        </section>
        <main id="main-content" tabIndex={-1} className="auth-card">
          {children}
        </main>
      </div>
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
