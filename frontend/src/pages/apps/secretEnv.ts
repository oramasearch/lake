/** An app's secret environment (lake#625): each value names one field of one secret in a lake
 * secret mount of the app's workspace, stored as `lake-secret://<mount id>/<name>#<field>`. The
 * operator projects it into the app when it is deployed and rewrites it when the secret changes. */

export type LakeSecretRef = { mount_id: string; name: string; field: string };

const scheme = "lake-secret://";

/** The mount, secret and field a stored reference names; null for anything else (an old
 * Kubernetes Secret name, say). The name may contain `/` and `#`; the field follows the last `#`. */
export function parseLakeSecretRef(reference: unknown): LakeSecretRef | null {
  if (typeof reference !== "string" || !reference.startsWith(scheme)) return null;
  const rest = reference.slice(scheme.length);
  const slash = rest.indexOf("/");
  const hash = rest.lastIndexOf("#");
  if (slash <= 0 || hash <= slash + 1 || hash === rest.length - 1) return null;
  return { mount_id: rest.slice(0, slash), name: rest.slice(slash + 1, hash), field: rest.slice(hash + 1) };
}

/** What the Environment table shows for a secret: the secret and its field, never a value. */
export function secretEnvLabel(reference: unknown): string {
  const parsed = parseLakeSecretRef(reference);
  return parsed ? `${parsed.name} → ${parsed.field}` : "Secret";
}

/** The set_app_env input for the Environment form: a secret when a mount is chosen, else a value. */
export function envInput(form: Record<string, string | undefined>): Record<string, unknown> {
  const key = (form.key ?? "").trim();
  const mount = (form.mount_id ?? "").trim();
  if (mount) {
    return { key, secret: { mount_id: mount, name: (form.secret_name ?? "").trim(), field: (form.field ?? "").trim() } };
  }
  return { key, value: form.value ?? "" };
}
