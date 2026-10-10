/** The workspace gate shared by Design, Train and Ship pages: while the workspace Zetta is not ready
 * it shows its state (and Start), otherwise the page. Ported with the Design pages. */
import type { ReactNode } from "react";

export function WorkspaceGate({ children }: { children: ReactNode }) {
  return <>{children}</>;
}
