/** The return-path check every sign-in page uses (LSR-184, lake#990). No imports, so it is tested
 * directly. */

/** A same-origin path: one leading slash, never `//host` or `/\host`, with no control character or
 * backslash anywhere (a browser drops tabs and newlines and reads `\` as `/`, which can turn an
 * accepted path into another host), and still on this origin once a URL parser has read it
 * (LSR-184, lake#990). */
export function sameOriginPath(value: string | null | undefined): value is string {
  if (!value || !value.startsWith("/") || value.startsWith("//") || /[\u0000-\u001f\u007f\\]/.test(value)) return false;
  const base = "https://console.invalid";
  try {
    return new URL(value, base).origin === base;
  } catch {
    return false;
  }
}
