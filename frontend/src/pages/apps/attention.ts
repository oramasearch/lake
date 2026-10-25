/** Which apps need attention on the Apps page (lake#1208). A Degraded app is crash-looping or
 * unavailable, so it is counted with Failed ones rather than read as fine. */

/** The phases a person must act on. Pending, Deploying and Stopped are on their way or chosen. */
export const ATTENTION_PHASES: readonly string[] = ["Failed", "Degraded"];

/** The `?filter=` value that shows only the apps needing attention. */
export const ATTENTION_FILTER = "attention";

/** Whether an app with this phase needs attention. */
export function needsAttention(phase?: string | null): boolean {
  return ATTENTION_PHASES.includes(phase ?? "");
}
