/** Code-split pages that survive a deploy. A tab opened before a roll references the previous
 * build's chunk names; after the roll those files are gone and `import()` fails. The first such
 * failure reloads the page once (fetching the new index and its chunks); a second failure within a
 * minute is thrown to the error boundary instead of looping. */
import { lazy, type ComponentType } from "react";

const KEY = "lake-chunk-reload";
const WINDOW_MS = 60_000;

export function lazyPage<T extends ComponentType<object>>(load: () => Promise<{ default: T }>) {
  return lazy(async () => {
    try {
      return await load();
    } catch (error) {
      let last = 0;
      try {
        last = Number(sessionStorage.getItem(KEY) || 0);
      } catch {
        /* storage blocked: one reload attempt */
      }
      if (Date.now() - last > WINDOW_MS) {
        try {
          sessionStorage.setItem(KEY, String(Date.now()));
        } catch {
          /* ignore */
        }
        window.location.reload();
        return new Promise<{ default: T }>(() => {});
      }
      throw error;
    }
  });
}
