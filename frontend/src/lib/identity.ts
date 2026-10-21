/**
 * Which key an account signs with (lake#762, docs/api/identity.md#your-own-identity): pure helpers
 * for Account → Identity. Pages say "managed key" and "own key"; never the store's word for it.
 */

export type AccountIdentity = {
    taap_id: string; key : "managed" | "own"; signs_as : string;
    managed_key : {public_key : string; held : boolean; destroyed_at : string | null} | null;
    linked : {own_taap_id : string; own_document_hash : string; linked_at : string} | null;
};

/** The one-line summary of the account's key. */
export function keySummary(identity: Pick<AccountIdentity, "key"|"linked"|"managed_key">): string {
    if (identity.key === "managed")
        return "Managed key: Lake holds a temporary key for you.";
    if (identity.linked)
        return "Your own key: the managed key was replaced and destroyed.";
    return "Your own key: you registered it when you signed up.";
}

/** A challenge for proving you hold an identity document's key before sign-up or a first OAuth
 * sign-in binds it to a new account (`begin_identity_proof`, lake#931). */
export type IdentityProofChallenge = {challenge : string; nonce : string; expires_at : number; command : string};

/** The command that signs the challenge with the person's liquid identity. */
export function signCommand(challenge: string): string {
    return `liquid sign artifact ${challenge}`;
}

/**
 * What to send back: everything `liquid sign` printed, trimmed. Lake reads the `signature=` line
 * (and checks `signer=`), or a bare hexadecimal signature.
 */
export function signedOutput(pasted: string): string {
    return pasted.trim();
}

/** Whether the pasted text looks like `liquid sign` output, before sending it. */
export function looksSigned(pasted: string): boolean {
    const text = pasted.trim();
    if (!text)
        return false;
    const line = text.split(/\r?\n/).find((l) => l.trim().startsWith("signature="));
    const hex = (line ? line.trim().slice("signature=".length) : text).trim();
    return /^[0-9a-fA-F]+$/.test(hex) && hex.length % 2 === 0;
}
