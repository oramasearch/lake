/** "Usage This Month" on an agent, app, service or model page (lake#564): what that one thing used
 * in its workspace since the first of the month, from `usage_over_time`, and a link to the Usage
 * dashboard for the workspace. A figure, not a price: billing is not designed. */
import { Gauge } from "lucide-react";
import { useRead } from "@/lib/api";
import { Button, Card, KeyValue, Skeleton } from "@/lib/ui";
import { formatValue, usageLink, usageOf, windows, type Reading } from "./usage";

export type UsageMatch = { dimension: string; breakdown: "by_object" | "by_agent" | "by_workspace" | "by_model"; keys: string[]; label?: string };

export function UsageThisMonth({ customerId, workspaceId, match, note }: { customerId: string; workspaceId: string; match: UsageMatch[]; note?: string }) {
  const month = windows("month").current;
  const reading = useRead<Reading>(customerId && workspaceId ? "usage_over_time" : null, {
    customer_id: customerId,
    workspace_ids: [workspaceId],
    dimensions: [...new Set(match.map((m) => m.dimension))],
    from: month.from,
    to: month.to,
    granularity: "month",
  }, { workspace: null, staleTime: 300_000 });
  const figures = usageOf(reading.data, match);
  return (
    <Card
      label="Usage This Month"
      actions={
        <Button variant="ghost" to={usageLink({ workspaces: [workspaceId], dimension: match[0]?.dimension })}>
          <Gauge size={14} aria-hidden /> Usage
        </Button>
      }
    >
      {reading.isLoading ? (
        <Skeleton lines={2} />
      ) : reading.isError ? (
        <p className="muted m-0 text-[13px]">Usage could not be read.</p>
      ) : (
        <>
          <KeyValue rows={figures.map((f, i) => [match[i].label ?? f.title, <span className="font-mono">{formatValue(f.value, f.unit)}</span>])} />
          {note && <p className="muted mb-0 text-[12px]">{note}</p>}
        </>
      )}
    </Card>
  );
}
