// SPDX-License-Identifier: MIT

import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { BrowserRouter } from "react-router-dom";
import { QueryClientProvider } from "@tanstack/react-query";
import App from "./App";
import { queryClient } from "./lib/api";
import { ToastProvider } from "./lib/toast";
import { captureCouponCode, queryWithoutCode } from "./lib/coupon";
// Imported on its own so Vite resolves and content-hashes the font files (inside index.css's
// layered @import chain the url()s were left as written and pointed nowhere).
import "./styles/fonts.css";
import "./index.css";

// A coupon code an ad's link carries (`?code=`, lake#1156): kept in this browser through sign-up and
// redeemed on the first signed-in page (CouponWelcome), and taken out of the address so a reload or
// a shared link does not carry it further.
try {
  if (captureCouponCode(window.location.search, window.localStorage)) {
    const { pathname, hash } = window.location;
    window.history.replaceState(window.history.state, "", `${pathname}${queryWithoutCode(window.location.search)}${hash}`);
  }
} catch {
  /* storage or history refused: the page still opens */
}

createRoot(document.getElementById("root")!).render(
  <StrictMode>
    <QueryClientProvider client={queryClient}>
      <ToastProvider>
        <BrowserRouter>
          <App />
        </BrowserRouter>
      </ToastProvider>
    </QueryClientProvider>
  </StrictMode>,
);
