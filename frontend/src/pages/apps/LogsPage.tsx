/** An environment's logs (web_ui.cpp web_ui_logs; serve_logs_page). The page reads
 * `environment_logs` with GET and polls it every 3 seconds from `next_since`, appending lines (the
 * server page's script POSTed to a GET-only read and never appended anything). */
import { useEffect, useState } from "react";
import { Link, Navigate, useSearchParams } from "react-router-dom";
import { read, type Row } from "@/lib/api";
import { ErrorBlock, Page, Skeleton } from "@/lib/ui";
import { usePlatformRepository } from "./EnvironmentsPage";
import { pause, RepositoryTabs, StaleBanner } from "./shared";

type LogPage = { items?: Row[]; next_since?: string; stale_since?: string | null };

function line(item: Row): string {
  return `${item.ts ?? ""} ${item.pod ?? ""}/${item.container ?? ""} ${item.line ?? ""}\n`;
}

export default function LogsPage() {
  const [params] = useSearchParams();
  const repositoryId = params.get("repository_id") ?? "";
  const environment = params.get("environment") ?? "";
  const since = params.get("since") ?? "";
  usePlatformRepository(repositoryId, `${environment} logs`);
  const [lines, setLines] = useState("");
  const [cursor, setCursor] = useState(since);
  const [stale, setStale] = useState<string | null>(null);
  const [error, setError] = useState<unknown>(null);
  const [loaded, setLoaded] = useState(false);

  useEffect(() => {
    if (!repositoryId || !environment) return;
    const controller = new AbortController();
    const signal = controller.signal;
    setLines("");
    setLoaded(false);
    setError(null);
    void (async () => {
      let next = since;
      let first = true;
      while (!signal.aborted) {
        try {
          const value = await read<LogPage>(
            "environment_logs",
            { repository_id: repositoryId, environment, since: next || undefined, limit: 200, direction: "forward" },
            { workspace: null, signal },
          );
          const items = value.items ?? [];
          if (items.length) setLines((current) => current + items.map(line).join(""));
          next = value.next_since || next;
          setCursor(next);
          setStale(value.stale_since ?? null);
          setError(null);
        } catch (e) {
          if (signal.aborted) break;
          // The first read's failure is the page's; a later one keeps the lines and retries.
          if (first) setError(e);
        }
        if (first) {
          first = false;
          setLoaded(true);
        }
        await pause(3000, signal);
      }
    })();
    return () => {
      controller.abort();
    };
  }, [repositoryId, environment, since]);

  if (!repositoryId || !environment) return <Navigate to="/repositories" replace />;
  return (
    <Page eyebrow="environment logs" title={environment}>
      <RepositoryTabs repositoryId={repositoryId} current="platform" />
      <StaleBanner since={stale} />
      {!loaded ? (
        <Skeleton />
      ) : error && !lines ? (
        <ErrorBlock error={error} what="logs" />
      ) : (
        <section className="panel" id="platform-logs">
          <h2>Logs</h2>
          <pre aria-live="polite">
            <code>{lines}</code>
          </pre>
          <p>
            <Link to={`/logs?repository_id=${encodeURIComponent(repositoryId)}&environment=${encodeURIComponent(environment)}&since=${encodeURIComponent(cursor)}`}>Refresh</Link>
          </p>
        </section>
      )}
    </Page>
  );
}
