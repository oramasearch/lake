// SPDX-License-Identifier: MIT

/** The Usage dashboard's chart: one dimension over time, stacked by workspace or by kind, drawn with
 * uPlot as Train and Ship's charts are, in lake's colours, redrawn when the theme changes. Each
 * band is a period's value stacked on the bands before it (stackSeries); the legend and the
 * tooltip show each band's own value. */
import { useEffect, useRef } from "react";
import uPlot from "uplot";
import "uplot/dist/uPlot.min.css";
import "@/styles/usage.css";
import { useThemeTick } from "@/pages/experiments/Chart";
import { formatValue } from "./usage";

function token(name: string): string {
  const sample = document.createElement("span");
  sample.style.color = `var(${name})`;
  document.body.append(sample);
  const color = getComputedStyle(sample).color;
  sample.remove();
  return color;
}

/** A colour with some transparency, for a band's fill. */
function fade(color: string, alpha: number): string {
  const match = color.match(/rgba?\(([^)]+)\)/);
  if (!match) return color;
  const [r, g, b] = match[1].split(",").map((part) => part.trim());
  return `rgba(${r}, ${g}, ${b}, ${alpha})`;
}

const PALETTE = ["--lake-action", "--lake-ai", "--lake-success", "--lake-warning", "--lake-info", "--lake-danger", "--lake-text-secondary"];

export function UsageChart({
  periods,
  labels,
  values,
  stacked,
  unit,
  title,
}: {
  periods: string[];
  labels: string[];
  values: (number | null)[][];
  stacked: (number | null)[][];
  unit: string;
  title: string;
}) {
  const target = useRef<HTMLDivElement>(null);
  const theme = useThemeTick();
  const empty = !periods.length || stacked.every((line) => line.every((v) => v === null || v === 0));
  useEffect(() => {
    const element = target.current;
    if (!element || empty) return;
    const colors = PALETTE.map(token);
    const text = token("--lake-text-secondary");
    const border = token("--lake-border-subtle");
    const xs = periods.map((p) => Date.parse(p) / 1000);
    const stepped = uPlot.paths.stepped?.({ align: 1 });
    const options: uPlot.Options = {
      width: Math.max(260, element.clientWidth),
      height: 260,
      series: [
        { label: "Period" },
        ...labels.map((label, index) => ({
          label,
          stroke: colors[index % colors.length],
          fill: fade(colors[index % colors.length], 0.28),
          width: 1.5,
          paths: stepped,
          points: { show: false },
          value: (_plot: uPlot, _v: number | null, _series: number, row: number | null) => (row === null ? "—" : formatValue(values[index]?.[row] ?? null, unit)),
        })),
      ],
      // Each band fills down to the band beneath it.
      bands: labels.slice(1).map((_, index) => ({ series: [index + 2, index + 1] as [number, number] })),
      scales: { x: { time: true }, y: { range: (_plot, _min, max) => [0, max > 0 ? max * 1.08 : 1] } },
      axes: [
        { stroke: text, grid: { stroke: border }, ticks: { stroke: border } },
        { stroke: text, grid: { stroke: border }, ticks: { stroke: border }, size: 70, values: (_plot, ticks) => ticks.map((tick) => formatValue(tick, unit)) },
      ],
      cursor: { drag: { x: false, y: false } },
    };
    const plot = new uPlot(options, [xs, ...stacked] as uPlot.AlignedData, element);
    const observer = new ResizeObserver(() => {
      const width = Math.max(260, element.clientWidth);
      if (width !== plot.width) plot.setSize({ width, height: 260 });
    });
    observer.observe(element);
    return () => {
      observer.disconnect();
      plot.destroy();
    };
  }, [periods, labels, values, stacked, unit, theme, empty]);
  if (empty) return <p className="muted m-0 text-[13px]">Nothing was used in this period.</p>;
  return <div ref={target} className="usage-chart" role="img" aria-label={`${title} over time. The table below has the same figures.`} />;
}
