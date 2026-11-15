// SPDX-License-Identifier: MIT

/** Lake's mark: the lake glyph in white on a rounded square in the action blue, the brand's
 * gradient running across it. */
export function LakeMark({ size = 28 }: { size?: number }) {
  return (
    <svg width={size} height={size} viewBox="0 0 32 32" aria-hidden focusable="false">
      <defs>
        <linearGradient id="lake-mark-g" x1="0" y1="0" x2="1" y2="1">
          <stop offset="0" stopColor="var(--orama-color-blue-50)" />
          <stop offset="1" stopColor="var(--orama-color-violet-60)" />
        </linearGradient>
      </defs>
      <rect width="32" height="32" rx="7" fill="url(#lake-mark-g)" />
      <g transform="translate(8 8)" fill="#fff">
        <path d="M4 3h8v2H6v6h6v2H4z" />
        <path d="M8 7h4v2H8z" />
      </g>
    </svg>
  );
}
