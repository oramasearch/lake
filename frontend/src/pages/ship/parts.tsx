// SPDX-License-Identifier: MIT

/** Pieces the Ship pages share: an inline copy button beside an identifier or address, and the
 * origin outside callers use. */
import { useState } from "react";
import { Check, Copy } from "lucide-react";
import { useSession } from "@/lib/session";
import { Mono } from "@/lib/ui";

/** A small icon button that copies `value` (an id, an address) and says so for a moment. */
export function CopyIcon({ value, label = "Copy" }: { value: string; label?: string }) {
  const [copied, setCopied] = useState(false);
  if (!value) return null;
  return (
    <button
      type="button"
      className="icon-button"
      title={copied ? "Copied" : label}
      aria-label={copied ? "Copied" : label}
      onClick={async () => {
        try {
          await navigator.clipboard.writeText(value);
          setCopied(true);
          setTimeout(() => setCopied(false), 1600);
        } catch {
          /* the value stays selectable */
        }
      }}
    >
      {copied ? <Check size={14} aria-hidden className="text-ok" /> : <Copy size={14} aria-hidden />}
    </button>
  );
}

/** An identifier in mono with a copy button after it. */
export function CopyMono({ value, short, label }: { value: string; short?: number; label?: string }) {
  if (!value) return <span className="muted">—</span>;
  return (
    <span className="inline-flex max-w-full items-center gap-1">
      <Mono short={short}>{value}</Mono>
      <CopyIcon value={value} label={label} />
    </span>
  );
}

/** The origin outside callers use: lake's configured external origin, its public host, else this
 * page's origin (the server page fell back to a fixed host). */
export function useOrigin(): string {
  const { viewer } = useSession();
  if (viewer.external_origin) return viewer.external_origin;
  if (viewer.public_host) return `https://${viewer.public_host}`;
  return window.location.origin;
}
