// SPDX-License-Identifier: MIT

/** Usage figures in words, as the server's usage page wrote them (usage_duration, usage_money,
 * usage_hour). Shared by Usage and an organization's Usage tab. */

/** "0 s", "45 s", "12 min 5 s", "3 h 4 min": compute time in plain units (usage_duration). */
export function usageDuration(value: unknown): string {
  const seconds = Math.max(0, Math.floor(Number(value) || 0));
  if (seconds < 60) return `${seconds} s`;
  if (seconds < 3600) return seconds % 60 === 0 ? `${seconds / 60} min` : `${Math.floor(seconds / 60)} min ${seconds % 60} s`;
  const hours = Math.floor(seconds / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  return minutes === 0 ? `${hours} h` : `${hours} h ${minutes} min`;
}

/** Integer micro-units of a currency as an amount a person reads: "12.35 USD", "less than 0.01
 * USD" (usage_money). */
export function usageMoney(micros: unknown, currency: string): string {
  const value = Math.max(0, Math.floor(Number(micros) || 0));
  if (value !== 0 && value < 10_000) return `less than 0.01 ${currency}`;
  const cents = Math.floor((value + 5_000) / 10_000);
  return `${Math.floor(cents / 100)}.${String(cents % 100).padStart(2, "0")} ${currency}`;
}

/** "2026-09-04T00:00:00Z" as "2026-09-04 00:00 UTC" (usage_hour). */
export function usageHour(hour: string): string {
  return hour.length >= 16 && hour[10] === "T" ? `${hour.slice(0, 10)} ${hour.slice(11, 16)} UTC` : hour;
}
