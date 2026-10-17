# Regions in the Web App

lake#757, decision 64. The API, the variables and the apex picker are in
[`../api/regions.md`](../api/regions.md).

## What exists

Where Lake runs in regions (production: `eu.lake.live`, `us.lake.live`), the web app shows which
region it is in. Each region is a separate Lake with its own accounts, so another region is always
a link to that region's host, never a switch inside this Lake. Everything is read from
`server_info`'s `region` and `regions`; a Lake without a region (nonprod, local, an enterprise
install) answers `null` and `[]`, and the app shows none of what follows.

| Where | What it shows | Code |
|---|---|---|
| The top bar, every page in the shell | The region's label with a globe icon (the label hides below `sm`). With other regions, a button opening a menu: one sentence ("You are in Europe. Each region is a separate Lake with its own accounts.") and a link per other region, `https://<host>/`, with its host | `RegionMenu`, `frontend/src/components/Region.tsx`, in `Shell.tsx` |
| Sign in (`/sign-in`, lake's own and Clerk's) | "You are signing in to Lake in Europe. An account belongs to the region it was created in.", then links to the other regions | `AccountRegionNote mode="sign-in"` in `SignInPage.tsx`, `ClerkSignIn.tsx` |
| Create an account (`/sign-up`, both) | "Your account is created in Europe. Each region is a separate Lake with its own accounts, and your workspaces stay in Europe.", then links to the other regions | `AccountRegionNote mode="sign-up"` in `SignUpPage.tsx`, `ClerkSignIn.tsx` |
| Ship → Workspaces | The page's sub-line ends "Workspaces here are in Europe."; each workspace's second line shows the region after its environment; **New workspace** says "Lake prepares it in Europe, where it stays" | `WorkspacesPage.tsx` |

The words come from the pure functions in `frontend/src/lib/regions.ts` (`currentRegion`,
`otherRegions`, `regionUrl`, `accountRegionSentence`, `workspaceRegionLabel`). `server_info` is read
once with `useRead("server_info", undefined, {workspace: null})` and kept for the page's life
(`staleTime: Infinity`): it changes only when the Lake restarts.

Why links to the host's front page and not the same path: the other region has other accounts and
other workspaces, so the current path (a workspace id, a repository) means nothing there.

## How to verify

- `cd frontend && npm test` runs `tests/regions.test.ts`: the current and other regions, a Lake
  without regions, the host links, and the sign-in, sign-up and workspace wording.
- In a browser, start a local Lake with the variables in
  [`../api/regions.md`](../api/regions.md#how-to-verify) and open `http://127.0.0.1:4900/sign-in`:
  the region sentence is under the title; signed in, the top bar shows **Europe** and its menu
  links to `https://us.lake.live/`. Without the variables none of it appears.

## How to change it

- The wording is in `frontend/src/lib/regions.ts`; change its test with it.
- A new place that should show the region reads `useServerRegions()` from
  `frontend/src/components/Region.tsx`.
