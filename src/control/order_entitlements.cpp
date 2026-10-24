#include "lake/control/store.hpp"

#include <algorithm>
#include <chrono>

namespace lake::control {

std::string cloud_order_entitlement_state(const CloudOrderEntitlement& entitlement,
                                          std::int64_t now) {
    if (entitlement.access_until <= 0) {
        return "suspended";
    }
    if (now < entitlement.access_until) {
        return "active";
    }
    return now < entitlement.access_until + entitlement.grace_seconds ? "grace" : "suspended";
}

bool valid_cloud_order_subscription_event(const CloudOrderSubscriptionEvent& event) {
    const auto epoch = [](std::int64_t value) { return value >= 0 && value <= 253402300799; };
    return event.provider == "stripe" && !event.event_id.empty() && event.event_id.size() <= 255 &&
           !event.subscription_id.empty() && event.subscription_id.size() <= 255 &&
           !event.customer_id.empty() && event.customer_id.size() <= 255 &&
           event.order_id.size() <= 255 && event.event_type.size() <= 128 &&
           !event.status.empty() && event.status.size() <= 64 && event.created > 0 &&
           epoch(event.created) && epoch(event.period_end) && epoch(event.next_payment_attempt) &&
           epoch(event.ended_at) && event.grace_seconds >= 0 && event.grace_seconds <= 30 * 86400 &&
           event.invoice_id.size() <= 255 &&
           (!event.event_type.starts_with("invoice.") || !event.invoice_id.empty());
}

bool apply_cloud_order_entitlement(CloudOrderEntitlement& entitlement,
                                   const CloudOrderSubscriptionEvent& event) {
    // Stripe can deliver snapshots out of order. A cancellation is terminal for this subscription;
    // restoring it requires a new purchase. For ambiguous same-second snapshots, deny access.
    const bool grants = event.status == "active" || event.status == "trialing";
    const bool was_denied = entitlement.subscription_status != "active" &&
                            entitlement.subscription_status != "trialing" &&
                            entitlement.subscription_status != "incomplete" &&
                            entitlement.subscription_status != "pending";
    if (entitlement.subscription_status == "canceled") {
        return false;
    }
    const bool new_payment =
        (event.event_type == "invoice.paid" || (event.event_type == "invoice.updated" && grants)) &&
        event.period_end > entitlement.paid_through;
    if ((event.created < entitlement.event_created && event.status != "canceled") ||
        (event.created == entitlement.event_created && grants && was_denied)) {
        // Retain paid-period evidence even when a newer status snapshot arrived first, but do
        // not let that older invoice reopen access denied by the newer subscription status.
        if (new_payment) {
            entitlement.paid_through = event.period_end;
        }
        return new_payment;
    }
    // A failed/voided invoice for an already paid period must not undo its successful collection.
    if (event.event_type.starts_with("invoice.") && !grants && event.period_end > 0 &&
        event.period_end <= entitlement.paid_through) {
        return false;
    }
    const bool retryable = entitlement.subscription_status == "active" ||
                           entitlement.subscription_status == "trialing" ||
                           entitlement.subscription_status == "past_due" ||
                           entitlement.subscription_status == "pending" ||
                           entitlement.subscription_status == "incomplete";
    if (event.status == "past_due" && !retryable) {
        // Collection attempts cannot relax an explicit unpaid/paused/unknown end state.
        entitlement.event_created = std::max(entitlement.event_created, event.created);
        return true;
    }
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    const bool expired = cloud_order_entitlement_state(entitlement, now) == "suspended";
    entitlement.event_created = std::max(entitlement.event_created, event.created);
    if (grants || !expired) {
        entitlement.grace_seconds = event.grace_seconds;
    }
    entitlement.subscription_status = event.status;
    if (grants) {
        entitlement.retry_invoice_id.clear();
        entitlement.access_until = event.period_end;
        if ((event.event_type == "invoice.paid" ||
             (event.event_type == "invoice.updated" && grants))) {
            entitlement.paid_through = std::max(entitlement.paid_through, event.period_end);
            entitlement.access_until = entitlement.paid_through;
        }
    } else if (event.status == "past_due") {
        // Do not treat the next unpaid billing period as paid. Only the signed retry deadline
        // can extend access beyond the last paid period.
        if (!event.invoice_id.empty()) {
            if (entitlement.retry_invoice_id.empty()) {
                entitlement.retry_invoice_id = event.invoice_id;
            }
            // A new unpaid invoice cannot restart dunning indefinitely, and another failed
            // attempt cannot restore compute that has already been suspended by the clock.
            if (!expired && entitlement.retry_invoice_id == event.invoice_id) {
                const auto previous = std::min(entitlement.access_until, event.created);
                entitlement.access_until =
                    std::max({entitlement.paid_through, event.next_payment_attempt, previous});
            }
        }
    } else {
        const auto end =
            event.ended_at > 0 ? std::min(event.ended_at, event.created) : event.created;
        if (entitlement.access_until > 0) {
            entitlement.access_until = std::min(entitlement.access_until, end);
        }
    }
    return true;
}

} // namespace lake::control
