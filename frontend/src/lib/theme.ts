/** The theme switch: `lake-theme` in localStorage and `data-theme` on <html>, as the server pages
 * kept it; with nothing stored the CSS follows the system preference. */
import { useState } from "react";

export function useTheme(): [string, () => void] {
  const current = () =>
    document.documentElement.dataset.theme || (matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light");
  const [theme, setTheme] = useState(current);
  const toggle = () => {
    const next = current() === "dark" ? "light" : "dark";
    document.documentElement.dataset.theme = next;
    try {
      localStorage.setItem("lake-theme", next);
    } catch {
      /* ignore */
    }
    setTheme(next);
  };
  return [theme, toggle];
}
