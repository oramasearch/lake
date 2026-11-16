// SPDX-License-Identifier: MIT

/** Whether Ship → Deployments asks a repository for its environments (lake#605). Pure, so
 * `npm test` covers it (tests/deployments.test.ts).
 *
 * A lake without the platform mirror answers every `list_environments` with `not_supported`, so the
 * page first reads `server_info`, whose `deployments` says whether the mirror is configured:
 *
 * - `false`: Deployments are not set up. The page asks no repository, and `server_info` is its one
 *   call.
 * - `true`: every repository is asked at once.
 * - not said (a lake from before lake#605, or `server_info` failed): the first repository is asked
 *   alone, and the others only once it answered something other than `not_supported`. */
import { isNotSetUp } from "../../lib/errors.ts";

export type ServerInfo = { deployments?: boolean };

/** What `server_info` says about Deployments: set up, not set up, or nothing. */
export function deploymentsSetUp(info: ServerInfo | undefined): boolean | undefined {
  return typeof info?.deployments === "boolean" ? info.deployments : undefined;
}

/** The first repository's read, as far as the decision needs it. */
export type FirstRead = { isSuccess: boolean; isError: boolean; error: unknown } | undefined;

/** Whether the repository at `index` is asked for its environments now. */
export function asksRepository(index: number, setUp: boolean | undefined, first: FirstRead): boolean {
  if (setUp === false) return false;
  if (setUp === true || index === 0) return true;
  if (!first) return false;
  return first.isSuccess || (first.isError && !isNotSetUp(first.error));
}
