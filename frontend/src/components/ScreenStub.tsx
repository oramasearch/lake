// SPDX-License-Identifier: MIT

/** A registered page whose port is in progress. */
import { Page, Empty } from "@/lib/ui";

export function stub(title: string) {
  return function Stub() {
    return (
      <Page title={title}>
        <Empty>This page is being moved to the new web app.</Empty>
      </Page>
    );
  };
}
