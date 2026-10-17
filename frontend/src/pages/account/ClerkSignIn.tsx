/** Sign-in and sign-up under Clerk (docs/api/clerk.md), ported from the server page's script
 * (web_ui_clerk_sign_in). clerk-js is loaded from the instance's own Frontend API host with the
 * publishable key; once Clerk has a session, its `lake` template token is posted once, as an
 * ordinary top-level form, to `POST /identity/clerk-session`, which verifies it, sets
 * `lake_session` and redirects. Lake reads no other Clerk state. */
import { useEffect, useRef, useState } from "react";
import { useLocation, useNavigate } from "react-router-dom";
import { Notice, Page } from "@/lib/ui";
import { useSession } from "@/lib/session";
import { AuthFrame } from "./AuthFrame";
import { AccountRegionNote } from "@/components/Region";
import { INVITATION_NOTE, returnQuery, returnTarget } from "./signIn";

/** The part of clerk-js this page uses. */
type ClerkSession = { getToken(options: { template: string }): Promise<string | null> };
type ClerkJs = {
  load(): Promise<void>;
  signOut(): Promise<void>;
  session: ClerkSession | null | undefined;
  addListener(listener: (state: { session?: ClerkSession | null }) => void): () => void;
  mountSignIn(node: HTMLDivElement, props: Record<string, unknown>): void;
  mountSignUp(node: HTMLDivElement, props: Record<string, unknown>): void;
  unmountSignIn(node: HTMLDivElement): void;
  unmountSignUp(node: HTMLDivElement): void;
};

declare global {
  interface Window {
    Clerk?: ClerkJs;
  }
}

/** The `lake` JWT template: aud "lake", the person's email and name (docs/api/clerk.md). */
const TOKEN_TEMPLATE = "lake";

let loading: Promise<ClerkJs> | null = null;

/** clerk-js from `https://<frontend_api>/npm/@clerk/clerk-js@5/dist/clerk.browser.js`, once. */
function loadClerk(publishableKey: string, frontendApi: string): Promise<ClerkJs> {
  if (window.Clerk) return Promise.resolve(window.Clerk);
  if (loading) return loading;
  loading = new Promise<ClerkJs>((resolve, reject) => {
    const script = document.createElement("script");
    script.async = true;
    script.crossOrigin = "anonymous";
    script.dataset.clerkPublishableKey = publishableKey;
    script.src = `https://${frontendApi}/npm/@clerk/clerk-js@5/dist/clerk.browser.js`;
    script.onload = () => (window.Clerk ? resolve(window.Clerk) : reject(new Error("Clerk did not start.")));
    script.onerror = () => {
      loading = null;
      reject(new Error("Clerk could not be reached."));
    };
    document.head.appendChild(script);
  });
  return loading;
}

/** Post the Clerk token as a real form: a top-level navigation, so the server's Set-Cookie and
 * redirect (or its `?error=`) apply to the page. `next` carries the pending return. */
function postToLake(token: string, next: string) {
  const form = document.createElement("form");
  form.method = "post";
  form.action = "/identity/clerk-session";
  const fields: [string, string][] = [
    ["token", token],
    ["device_name", "browser"],
  ];
  if (next) fields.push(["next", next]);
  for (const [name, value] of fields) {
    const input = document.createElement("input");
    input.type = "hidden";
    input.name = name;
    input.value = value;
    form.append(input);
  }
  document.body.append(form);
  form.submit();
}

/** Clerk's form inside lake's card: lake's blue, IBM Plex and radius, and no card of its own. */
const CLERK_APPEARANCE = {
  variables: { colorPrimary: "#0f62fe", fontFamily: "'IBM Plex Sans', 'Helvetica Neue', Arial, sans-serif", borderRadius: "6px" },
  elements: {
    rootBox: { width: "100%" },
    cardBox: { width: "100%", boxShadow: "none", border: "0" },
    card: { boxShadow: "none", border: "0", padding: "0", background: "transparent" },
    header: { display: "none" },
  },
};

export function ClerkSignIn({ mode }: { mode: "sign-in" | "sign-up" }) {
  const { viewer } = useSession();
  const location = useLocation();
  const navigate = useNavigate();
  const mount = useRef<HTMLDivElement>(null);
  const [status, setStatus] = useState("");
  // The server's error stays on the page after the address drops it.
  const [error] = useState(() => new URLSearchParams(location.search).get("error"));
  const [signedOut] = useState(() => !!new URLSearchParams(location.search).get("signed_out"));
  const kept = returnQuery(location.search);
  const next = returnTarget(location.search, "");
  const signUp = mode === "sign-up";

  useEffect(() => {
    const node = mount.current;
    const key = viewer.clerk.publishable_key;
    const host = viewer.clerk.frontend_api;
    if (!node || !key || !host) {
      setStatus("Sign-in could not load: Clerk is not configured on this Lake.");
      return;
    }
    let cancelled = false;
    let clerk: ClerkJs | null = null;
    let stopListening: (() => void) | null = null;
    let busy = false;
    const exchange = async () => {
      if (busy || cancelled || !clerk?.session) return;
      busy = true;
      setStatus("Signing you in.");
      try {
        const token = await clerk.session.getToken({ template: TOKEN_TEMPLATE });
        if (!token) throw new Error("Clerk gave no token.");
        postToLake(token, next);
      } catch (e) {
        busy = false;
        setStatus(`Sign-in could not load: ${(e as Error).message}`);
      }
    };
    void (async () => {
      try {
        clerk = await loadClerk(key, host);
        await clerk.load();
        if (cancelled) return;
        if (signedOut) {
          // Without this, the still-live Clerk session would sign the person straight back in.
          await clerk.signOut();
          navigate({ pathname: location.pathname, search: kept }, { replace: true });
        }
        if (clerk.session) {
          await exchange();
          return;
        }
        stopListening = clerk.addListener(({ session }) => {
          if (session) void exchange();
        });
        // Clerk returns to this page, keeping the pending return, and the exchange runs here.
        const back = `/sign-in${kept}`;
        if (signUp) clerk.mountSignUp(node, { signInUrl: back, forceRedirectUrl: back, appearance: CLERK_APPEARANCE });
        else clerk.mountSignIn(node, { signUpUrl: `/sign-up${kept}`, forceRedirectUrl: back, appearance: CLERK_APPEARANCE });
      } catch (e) {
        if (!cancelled) setStatus(`Sign-in could not load: ${(e as Error).message}`);
      }
    })();
    return () => {
      cancelled = true;
      stopListening?.();
      if (clerk) {
        try {
          if (signUp) clerk.unmountSignUp(node);
          else clerk.unmountSignIn(node);
        } catch {
          /* not mounted */
        }
      }
    };
    // The page mounts Clerk once; the address change after signing out must not remount it.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [mode, viewer.clerk.publishable_key, viewer.clerk.frontend_api]);

  return (
    <AuthFrame title={signUp ? "create account" : "sign in"}>
      <Page
        eyebrow="Lake by Orama"
        title={signUp ? "Create Your Account" : "Sign In"}
        sub={
          signUp
            ? `${INVITATION_NOTE} Ask the person who invited you, or sign in if you already have access.`
            : "Sign in with the email address you were invited with."
        }
      >
        <AccountRegionNote mode={signUp ? "sign-up" : "sign-in"} />
        {error && <Notice tone="danger">{error}</Notice>}
        <div ref={mount} className="clerk-mount" />
        <p className="muted" role="status" aria-live="polite">
          {status}
        </p>
      </Page>
    </AuthFrame>
  );
}
