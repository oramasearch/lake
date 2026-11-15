// SPDX-License-Identifier: MIT

/** Lake's icon set (16×16, currentColor), the same shapes the server-rendered pages used. */
import type { ReactElement } from "react";

const PATHS: Record<string, string[]> = {
  lake: ["M4 3h8v2H6v6h6v2H4z", "M8 7h4v2H8z"],
  home: ["M8 1.4 1.5 7v7.5h5V10h3v4.5h5V7L8 1.4Zm4.5 11.1h-1V8h-7v4.5h-1V7.9L8 4l4.5 3.9v4.6Z"],
  repository: ["M3 1h9.5A1.5 1.5 0 0 1 14 2.5V14H3a2 2 0 0 1-2-2V3a2 2 0 0 1 2-2Zm0 2v7h9V3H3Zm0 9a.5.5 0 0 0 0 1h9v-1H3Z"],
  search: ["m14.7 13.3-3.1-3.1a5.5 5.5 0 1 0-1.4 1.4l3.1 3.1 1.4-1.4ZM3 7.5A4.5 4.5 0 1 1 12 7.5a4.5 4.5 0 0 1-9 0Z"],
  customers: ["M8 8a3 3 0 1 0 0-6 3 3 0 0 0 0 6Zm0-2a1 1 0 1 1 0-2 1 1 0 0 1 0 2Zm0 3c-3.3 0-6 1.8-6 4v1h12v-1c0-2.2-2.7-4-6-4Zm-3.5 3c.7-.6 2-1 3.5-1s2.8.4 3.5 1h-7Z"],
  organization: ["M2 1h9v4h3v10H2V1Zm2 2v10h2v-2h2v2h1V3H4Zm7 4v6h1V7h-1Z", "M5 5h1v1H5zm2 0h1v1H7zM5 8h1v1H5zm2 0h1v1H7z"],
  metering: ["M2 2h12v12H2V2Zm2 2v8h8V4H4Zm1 6h1V8H5v2Zm2 0h1V5H7v5Zm2 0h1V7H9v3Z"],
  secrets: ["M4 7V5a4 4 0 0 1 8 0v2h2v8H2V7h2Zm2 0h4V5a2 2 0 0 0-4 0v2Zm-2 2v4h8V9H4Z"],
  audit: ["M3 1h7l3 3v11H3V1Zm2 2v10h6V5H9V3H5Zm1 5 1.4 1.4L10.8 6 12 7.2 7.4 12 4.8 9.4 6 8Z"],
  identity: ["M8 1a4 4 0 1 0 0 8 4 4 0 0 0 0-8Zm0 2a2 2 0 1 1 0 4 2 2 0 0 1 0-4Zm0 7c-3.3 0-6 1.8-6 4v1h12v-1c0-2.2-2.7-4-6-4Zm-3.5 3c.7-.6 2-1 3.5-1s2.8.4 3.5 1h-7Z"],
  api: ["M1 2h14v12H1V2Zm2 2v8h10V4H3Zm1 2 2 2-2 2 1.4 1.4L8.8 8 5.4 4.6 4 6Zm5 4h3V8H9v2Z"],
  agent: ["M2 2h12v9H7l-3 3v-3H2V2Zm2 2v5h8V4H4Z", "M6 5.5h1v1.5H6zm3 0h1v1.5H9z"],
  streams: ["M1 3h9v2H1V3Zm0 4h12v2H1V7Zm0 4h9v2H1v-2Z", "m10 1 4 3-4 3V1Zm3 8 3 3-3 3V9Z"],
  docs: ["M1 3c2.5-1 5-1 7 .5C10 2 12.5 2 15 3v10c-2.5-1-5-1-7 .5C6 12 3.5 12 1 13V3Zm2 1.3v6.5c1.4-.3 2.8-.2 4 .4V4.7C5.8 4 4.4 4 3 4.3Zm10 0c-1.4-.3-2.8-.3-4 .4v6.5c1.2-.6 2.6-.7 4-.4V4.3Z"],
  theme: ["M8 1a7 7 0 1 0 7 7 7 7 0 0 0-7-7Zm0 12V3a5 5 0 0 1 0 10Z"],
};

export type IconName = keyof typeof PATHS;

export function Icon({ name, className = "icon" }: { name: string; className?: string }): ReactElement | null {
  const paths = PATHS[name];
  if (!paths) return null;
  return (
    <svg className={className} viewBox="0 0 16 16" aria-hidden="true" focusable="false">
      {paths.map((d) => (
        <path key={d} d={d} />
      ))}
    </svg>
  );
}
