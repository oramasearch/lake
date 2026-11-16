// SPDX-License-Identifier: MIT

/**
 * The Clerk sign-in page's decisions, kept pure so `npm test` can pin them (lake#1115, lake#1116).
 * The page itself is `pages/account/ClerkSignIn.tsx`; the server side is `POST
 * /identity/clerk-session` (docs/api/clerk.md).
 */

/**
 * Clerk's sign-up mode, as its environment tells clerk-js (`userSettings.signUp.mode`), or
 * "unknown" until clerk-js has loaded or when the field is missing.
 */
export type SignUpMode = "public"|"restricted"|"waitlist"|"unknown";

/**
 * The part of clerk-js's loaded environment this reads. `__unstable__environment` is how clerk-js
 * 5 exposes the instance settings it fetched from the Frontend API.
 */
export type ClerkEnvironmentHolder = {
    __unstable__environment?: {userSettings?: {signUp?: {mode?: unknown}}}|null;
};

/** The instance's sign-up mode, read from what clerk-js already fetched, never a second setting. */
export function signUpModeOf(clerk: ClerkEnvironmentHolder|null|undefined): SignUpMode {
    const mode = clerk?.__unstable__environment?.userSettings?.signUp?.mode;
    return mode === "public" || mode === "restricted" || mode === "waitlist" ? mode : "unknown";
}

/**
 * Whether people need an invitation: Clerk's restricted mode, and its waitlist, which admits
 * people one by one. Public, or not known yet, says nothing about invitations.
 */
export function signUpByInvitation(mode: SignUpMode): boolean {
    return mode === "restricted" || mode === "waitlist";
}

/** The invitation sentence, shown only where sign-up is by invitation (lake#1116). */
export const INVITATION_NOTE = "Accounts are added by invitation while Lake is in development.";

/** The line under the page title. */
export function clerkPageSub(signUp: boolean, mode: SignUpMode): string {
    if (signUp) {
        return signUpByInvitation(mode)
                   ? `${INVITATION_NOTE} Ask the person who invited you, or sign in if you already have access.`
                   : "Create your Lake account with your email address or one of the accounts below.";
    }
    return signUpByInvitation(mode)
               ? "Sign in with the email address you were invited with."
               : "Sign in with your email address or one of the accounts below.";
}

/**
 * Whether the page may post Clerk's token to Lake by itself. Not when Lake has just refused one
 * (`?error=`): Clerk's session is still live, so posting again would refuse again, once a second,
 * until the rate limit (lake#1115). The person retries with the page's button instead.
 */
export function exchangeAutomatically(search: string): boolean {
    return !new URLSearchParams(search).get("error");
}
