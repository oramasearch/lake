// SPDX-License-Identifier: MIT

/** The welcome for a coupon code carried through sign-up (lake#1156, docs/ui/balance.md#coupon-codes):
 * on the first signed-in page after a `?code=` link (main.tsx keeps the code), redeems it once for
 * the person's own account through `redeem_coupon` and says what it gave ("You have $50.00 of
 * credit from Orama.") or why it did not. Nothing where the lake does not bill, for the operator, or
 * while no code is waiting. */
import { useEffect, useRef, useState } from "react";
import { Gift, X } from "lucide-react";
import { act, ApiError } from "@/lib/api";
import { forgetCouponCode, pendingCouponCode, personalAccount, redemptionIsSettled } from "@/lib/coupon";
import { useSession } from "@/lib/session";
import { Banner, Button } from "@/lib/ui";

type Notice = { tone: "ok" | "warn"; title: string; text: string };

export function CouponWelcome() {
  const { viewer, refresh } = useSession();
  const [notice, setNotice] = useState<Notice | null>(null);
  const started = useRef(false);
  const account = viewer.signed_in && viewer.billing && !viewer.administrator ? personalAccount(viewer.spaces) : null;
  useEffect(() => {
    if (!account || started.current) return;
    let storage: Storage;
    try {
      storage = window.localStorage;
    } catch {
      return;
    }
    const code = pendingCouponCode(storage);
    if (!code) return;
    started.current = true;
    act<{ message: string }>("redeem_coupon", { customer_id: account, code }, { workspace: null })
      .then((redeemed) => {
        forgetCouponCode(storage);
        setNotice({ tone: "ok", title: "Welcome to Lake", text: redeemed.message });
        refresh();
      })
      .catch((error: unknown) => {
        const answer = error instanceof ApiError ? error : null;
        if (redemptionIsSettled(answer)) forgetCouponCode(storage);
        setNotice({
          tone: "warn",
          title: `Code ${code} Was Not Applied`,
          text: answer?.message ?? "Lake could not be reached. The code is tried again on your next page.",
        });
      });
  }, [account, refresh]);
  if (!notice) return null;
  return (
    <div className="page-container pb-0" data-testid="coupon-welcome">
      <Banner
        tone={notice.tone}
        title={
          <span className="inline-flex items-center gap-1.5">
            <Gift size={14} aria-hidden /> {notice.title}
          </span>
        }
        action={
          <Button variant="ghost" onClick={() => setNotice(null)} aria-label="Dismiss">
            <X size={14} aria-hidden />
          </Button>
        }
      >
        {notice.text}
      </Banner>
    </div>
  );
}
