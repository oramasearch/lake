import type { Screen } from "@/routes/types";
import { stub } from "@/components/ScreenStub";

export const screens: Screen[] = [
  { path: "/login", title: "Operator Sign In", section: "none", audience: ["visitor", "account", "operator"], bare: true, component: stub("Operator Sign In") },
  { path: "/sign-in", title: "Sign In", section: "none", audience: ["visitor", "account", "operator"], bare: true, component: stub("Sign In") },
  { path: "/sign-up", title: "Create Account", section: "none", audience: ["visitor", "account", "operator"], bare: true, component: stub("Create Account") },
  { path: "/passkey-sign-in", title: "Sign In with a Passkey", section: "none", audience: ["visitor", "account", "operator"], bare: true, component: stub("Sign In with a Passkey") },
  { path: "/mfa", title: "Multi-Factor Authentication", section: "account", audience: ["visitor", "account", "operator"], component: stub("Multi-Factor Authentication") },
  { path: "/settings", title: "Settings", section: "account", audience: ["account"], component: stub("Settings") },
  { path: "/sessions", title: "Sessions", section: "account", audience: ["account"], component: stub("Sessions") },
  { path: "/passkeys", title: "Passkeys", section: "account", audience: ["account"], component: stub("Passkeys") },
  { path: "/tokens", title: "Tokens", section: "account", audience: ["account"], component: stub("Tokens") },
];
