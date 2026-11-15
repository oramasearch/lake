// SPDX-License-Identifier: MIT

/** The value, once it has stopped changing for `ms` (a search box's query, before it is sent). */
import { useEffect, useState } from "react";

export function useSettled<T>(value: T, ms: number): T {
  const [settled, setSettled] = useState(value);
  useEffect(() => {
    const timer = setTimeout(() => setSettled(value), ms);
    return () => clearTimeout(timer);
  }, [value, ms]);
  return settled;
}
