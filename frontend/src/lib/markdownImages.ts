// SPDX-License-Identifier: MIT

/** Markdown images never load by themselves (lake#943). A reply an agent wrote, or a README, issue
 * or comment someone else wrote, may name any address as an image, and a browser that fetched it
 * would tell that server whatever the address carries: an agent misled by content it read could put
 * workspace data there. So every image renders as text, "Image: <alt>", with its address in the
 * tooltip, and the browser requests nothing. The console's `Content-Security-Policy: img-src` also
 * refuses images from other origins (docs/serving/http.md#images). `Markdown` (markdown.tsx) uses
 * this renderer. */
import { createElement } from "react";
import type { Components } from "react-markdown";

export const inertImages: Components = {
  img: ({ src, alt }) =>
    createElement(
      "span",
      { className: "markdown-image", title: src ? `Not loaded: ${src}` : undefined },
      alt?.trim() ? `Image: ${alt.trim()}` : "Image",
    ),
};
