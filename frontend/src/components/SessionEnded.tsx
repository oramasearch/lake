// SPDX-License-Identifier: MIT

/** "Your session has ended". A 401 anywhere, on a read or a write, means the session expired or was
 * revoked. The page stays where it is (what the person typed is still on it); this notice offers
 * Sign in again, which comes back to the same address. */
import { useEffect, useState } from "react";
import { onSessionEnded } from "@/lib/api";
import { Button } from "@/lib/ui";

export function signInAgainHref(): string {
  const here = window.location.pathname + window.location.search;
  return `/sign-in?next=${encodeURIComponent(here)}`;
}

export function SessionEndedNotice() {
  const [ended, setEnded] = useState(false);
  useEffect(() => {
    return onSessionEnded(() => setEnded(true));
  }, []);
  if (!ended) return null;
  return (
    <div className="session-ended" role="alert">
      <div>
        <strong className="block">Your session has ended</strong>
        <span className="muted">Sign in again to carry on. You'll come back to this page.</span>
      </div>
      <Button variant="primary" href={signInAgainHref()}>
        Sign in again
      </Button>
    </div>
  );
}
