import type { Screen } from "@/routes/types";
import { lazyPage } from "@/lib/lazyPage";

export const screens: Screen[] = [
  { path: "/login", title: "Operator Sign In", section: "none", audience: ["visitor", "account", "operator"], bare: true, component: lazyPage(() => import("./LoginPage")) },
  { path: "/sign-in", title: "Sign In", section: "none", audience: ["visitor", "account", "operator"], bare: true, component: lazyPage(() => import("./SignInPage")) },
  { path: "/sign-up", title: "Create Account", section: "none", audience: ["visitor", "account", "operator"], bare: true, component: lazyPage(() => import("./SignUpPage")) },
  {
    path: "/passkey-sign-in",
    title: "Sign In with a Passkey",
    section: "none",
    audience: ["visitor", "account", "operator"],
    bare: true,
    component: lazyPage(() => import("./PasskeySignInPage")),
  },
  { path: "/mfa", title: "Multi-Factor Authentication", section: "account", audience: ["visitor", "account", "operator"], component: lazyPage(() => import("./MfaPage")) },
  { path: "/settings", title: "Settings", section: "account", audience: ["account"], component: lazyPage(() => import("./SettingsPage")) },
  { path: "/sessions", title: "Sessions", section: "account", audience: ["account"], component: lazyPage(() => import("./SessionsPage")) },
  { path: "/passkeys", title: "Passkeys", section: "account", audience: ["account"], component: lazyPage(() => import("./PasskeysPage")) },
  { path: "/tokens", title: "Tokens", section: "account", audience: ["account"], component: lazyPage(() => import("./TokensPage")) },
];
