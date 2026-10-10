/** The passkey ceremonies, ported as they were from the server pages' inline script
 * (web_ui_passkeys, web_ui_passkey_sign_in): the same `/v1` calls, the same two conversions of the
 * options lake sends, the same fields back. Only the transport changed: `act` instead of a bare
 * fetch, so a failed call throws the API's message just as `if(!a.ok)throw Error(...)` did. */
import { act, type Row } from "@/lib/api";
import { NO_WORKSPACE } from "./signIn";

/** base64url → bytes. */
export function fromBase64Url(s: string): Uint8Array<ArrayBuffer> {
  return Uint8Array.from(atob(s.replace(/-/g, "+").replace(/_/g, "/") + "===".slice((s.length + 3) % 4)), (c) => c.charCodeAt(0));
}

/** bytes → unpadded base64url. */
export function toBase64Url(a: ArrayBuffer): string {
  return btoa(String.fromCharCode(...new Uint8Array(a)))
    .replace(/\+/g, "-")
    .replace(/\//g, "_")
    .replace(/=+$/, "");
}

/** Register a passkey for the signed-in account (`/passkeys`). */
export async function registerPasskey(name: string): Promise<void> {
  const a = await act<{ public_key: Row }>("begin_passkey_registration", {}, NO_WORKSPACE);
  const o = a.public_key;
  const c = o.challenge as string; // keep the base64url challenge string
  o.challenge = fromBase64Url(o.challenge);
  o.user.id = fromBase64Url(o.user.id); // only these two are converted
  const k = (await navigator.credentials.create({ publicKey: o as PublicKeyCredentialCreationOptions })) as PublicKeyCredential;
  const response = k.response as AuthenticatorAttestationResponse;
  await act(
    "register_passkey",
    {
      challenge: c,
      client_data_json: toBase64Url(response.clientDataJSON),
      attestation_object: toBase64Url(response.attestationObject),
      name,
    },
    NO_WORKSPACE,
  );
}

/** Sign in with a discoverable passkey (`/passkey-sign-in`). The answer sets `lake_session`. */
export async function signInWithPasskey(deviceName: string): Promise<void> {
  const a = await act<{ public_key: Row }>("begin_passkey_sign_in", {}, NO_WORKSPACE);
  const o = a.public_key;
  const c = o.challenge as string;
  o.challenge = fromBase64Url(o.challenge);
  const k = (await navigator.credentials.get({ publicKey: o as PublicKeyCredentialRequestOptions })) as PublicKeyCredential;
  const response = k.response as AuthenticatorAssertionResponse;
  await act(
    "sign_in_passkey",
    {
      challenge: c,
      credential_id: toBase64Url(k.rawId),
      client_data_json: toBase64Url(response.clientDataJSON),
      authenticator_data: toBase64Url(response.authenticatorData),
      signature: toBase64Url(response.signature),
      device_name: deviceName,
    },
    NO_WORKSPACE,
  );
}
