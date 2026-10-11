/** An environment's logs (web_ui.cpp web_ui_logs; serve_logs_page), in a terminal panel. The page
 * reads `environment_logs` with GET and polls it every 3 seconds from `next_since`, appending lines
 * (the server page's script POSTed to a GET-only read and never appended anything). */
import { useEffect, useState } from "react";
import { Link, Navigate, useSearchParams } from "react-router-dom";
import { ArrowLeft, RefreshCw } from "lucide-react";
import { read, type Row } from "@/lib/api";
import { Button, ErrorBlock, Page, Skeleton } from "@/lib/ui";
import { usePlatformRepository } from "./EnvironmentsPage";
import { pause, RepositoryTabs, StaleBanner, Terminal } from "./shared";
import { stopsPolling } from "@/lib/errors";

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
          // The first read's failure is the page's; a later one keeps the lines and retries. An
          // answer asking again cannot change (logs not set up, refused) ends the follow.
          if (first || stopsPolling(e)) setError(e);
          if (stopsPolling(e)) {
            setLoaded(true);
            break;
          }
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
  const environmentHref = `/environment?repository_id=${encodeURIComponent(repositoryId)}&name=${encodeURIComponent(environment)}`;
  const refreshHref = `/logs?repository_id=${encodeURIComponent(repositoryId)}&environment=${encodeURIComponent(environment)}&since=${encodeURIComponent(cursor)}`;
  return (
    <Page
      eyebrow="Data · Environment logs"
      title={environment}
      sub="The environment's container logs, followed every 3 seconds."
      actions={
        <>
          <Button to={environmentHref}>
            <ArrowLeft size={14} aria-hidden /> Environment
          </Button>
          <Button to={refreshHref}>
            <RefreshCw size={14} aria-hidden /> Refresh
          </Button>
        </>
      }
    >
      <RepositoryTabs repositoryId={repositoryId} current="platform" />
      <StaleBanner since={stale} />
      {!loaded ? (
        <Skeleton />
      ) : error && !lines ? (
        <ErrorBlock error={error} what="logs" />
      ) : (
        <section id="platform-logs" aria-label="Logs">
          <Terminal title={`${environment} · logs`} live actions={<Link to={refreshHref} className="text-inherit">Refresh</Link>} empty="No log lines yet. New lines appear here as they are written.">
            {lines}
          </Terminal>
        </section>
      )}
    </Page>
  );
}
