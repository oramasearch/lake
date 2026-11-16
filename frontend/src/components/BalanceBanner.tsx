// SPDX-License-Identifier: MIT

/** The prepaid balance's banner (lake#1114, docs/ui/balance.md): on every page while the current
 * account's balance is low, negative or blocked, with the balance, how much to add and Add funds,
 * which opens the one Add funds page. Reads `viewer.balance`; nothing where the lake does not bill,
 * for the operator, or while a paid order covers the account. */
import { useLocation } from "react-router-dom";
import { Wallet } from "lucide-react";
import { useSession } from "@/lib/session";
import { Banner, Button } from "@/lib/ui";
import { addFundsPath, bannerFor } from "@/pages/billing/balance";

export function BalanceBanner() {
  const { viewer } = useSession();
  const location = useLocation();
  const balance = viewer.billing ? viewer.balance : null;
  const banner = bannerFor(balance);
  // The Add funds page says all of it already.
  if (!banner || !balance || location.pathname === "/billing/add-funds") return null;
  return (
    <div className="page-container pb-0" data-testid="balance-banner">
      <Banner
        tone={banner.tone}
        title={banner.title}
        action={
          <Button variant="primary" to={addFundsPath(balance.customer_id)}>
            <Wallet size={14} aria-hidden /> Add funds
          </Button>
        }
      >
        {banner.text}
      </Banner>
    </div>
  );
}
