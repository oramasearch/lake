/** One uPlot chart in lake's colours (`assets/experiments/experiments.js` createChart): drag a
 * region to zoom on X, Shift-drag to pan, a resize observer, and a redraw when the theme changes.
 * Metric charts register themselves so the toolbar's zoom and pan act on all of them, and keep
 * their X view across refreshes until the axis changes. */
import { createContext, useContext, useEffect, useRef, useState } from "react";
import uPlot from "uplot";
import "uplot/dist/uPlot.min.css";

export type ChartSettings = { title?: string; xLabel?: string; time?: boolean; log?: boolean; metric?: boolean; xValues?: string[] };

/** The metric charts on a page and their remembered X views. */
export type ChartRegistry = { plots: Map<string, uPlot>; views: Map<string, { min: number; max: number }> };

export const ChartContext = createContext<ChartRegistry | null>(null);

export function newRegistry(): ChartRegistry {
  return { plots: new Map(), views: new Map() };
}

/** A lake token as a colour the canvas can draw (computed custom properties may still hold var()). */
function token(name: string): string {
  const sample = document.createElement("span");
  sample.style.color = `var(${name})`;
  document.body.append(sample);
  const color = getComputedStyle(sample).color;
  sample.remove();
  return color;
}

function palette(): string[] {
  return ["--lake-action", "--lake-ai", "--lake-success", "--lake-warning", "--lake-danger", "--lake-info", "--lake-text-primary", "--lake-text-secondary"].map(token);
}

/** A counter that changes when the light/dark theme does, so charts redraw in its colours. */
export function useThemeTick(): number {
  const [tick, setTick] = useState(0);
  useEffect(() => {
    const bump = () => setTick((n) => n + 1);
    const observer = new MutationObserver(bump);
    observer.observe(document.documentElement, { attributes: true, attributeFilter: ["data-theme"] });
    const media = matchMedia("(prefers-color-scheme: dark)");
    media.addEventListener("change", bump);
    return () => {
      observer.disconnect();
      media.removeEventListener("change", bump);
    };
  }, []);
  return tick;
}

export function Chart({ chartKey, data, labels, settings = {} }: { chartKey: string; data: (number | null)[][]; labels: string[]; settings?: ChartSettings }) {
  const target = useRef<HTMLDivElement>(null);
  const registry = useContext(ChartContext);
  const theme = useThemeTick();
  const empty = !data[0]?.length || !labels.length;
  const settingsKey = JSON.stringify(settings);
  useEffect(() => {
    const element = target.current;
    if (!element || empty) return;
    const colors = palette();
    const text = token("--lake-text-secondary");
    const border = token("--lake-border-subtle");
    let ready = false;
    let panning: { x: number; min: number; max: number } | null = null;
    const options: uPlot.Options = {
      width: Math.max(260, element.clientWidth),
      height: 280,
      series: [
        { label: settings.xLabel || "Step" },
        ...labels.map((label, index) => ({
          label,
          stroke: colors[index % colors.length],
          width: 2,
          dash: index >= 6 ? [6, 3] : [],
          spanGaps: false,
          points: { show: (data[0]?.length ?? 0) < 50, size: 5 },
        })),
      ],
      scales: { x: { time: settings.time || false }, y: { distr: settings.log ? 3 : 1 } },
      axes: [
        { stroke: text, grid: { stroke: border }, ticks: { stroke: border } },
        { stroke: text, grid: { stroke: border }, ticks: { stroke: border } },
      ],
      cursor: {
        drag: { x: true, y: false },
        bind: {
          mousedown: (_plot, _target, handler) => (event) => {
            if (!event.shiftKey) handler(event);
            return null;
          },
        },
      },
      hooks: {
        setScale: [
          (plot, scale) => {
            if (ready && scale === "x" && settings.metric && registry) {
              registry.views.set(chartKey, { min: plot.scales.x.min ?? 0, max: plot.scales.x.max ?? 0 });
            }
          },
        ],
      },
    };
    if (settings.xValues) {
      const names = settings.xValues;
      options.axes![0].values = (_plot, ticks) => ticks.map((value) => names[value] || "");
      options.axes![0].incrs = [1];
      options.series[0].value = (_plot, value) => (names[value] || value) as unknown as string;
    }
    const plot = new uPlot(options, data as uPlot.AlignedData, element);
    const saved = registry?.views.get(chartKey);
    if (saved && settings.metric) plot.setScale("x", saved);
    ready = true;
    const over = plot.over;
    const down = (event: PointerEvent) => {
      if (!event.shiftKey) return;
      panning = { x: event.clientX, min: plot.scales.x.min ?? 0, max: plot.scales.x.max ?? 0 };
      over.setPointerCapture(event.pointerId);
      event.preventDefault();
    };
    const move = (event: PointerEvent) => {
      if (!panning) return;
      const offset = ((event.clientX - panning.x) / over.clientWidth) * (panning.max - panning.min);
      plot.setScale("x", { min: panning.min - offset, max: panning.max - offset });
    };
    const up = () => {
      panning = null;
    };
    over.addEventListener("pointerdown", down);
    over.addEventListener("pointermove", move);
    over.addEventListener("pointerup", up);
    over.addEventListener("pointercancel", up);
    const observer = new ResizeObserver(() => {
      const width = Math.max(260, element.clientWidth);
      if (width !== plot.width) plot.setSize({ width, height: 280 });
    });
    observer.observe(element);
    if (settings.metric) registry?.plots.set(chartKey, plot);
    return () => {
      observer.disconnect();
      over.removeEventListener("pointerdown", down);
      over.removeEventListener("pointermove", move);
      over.removeEventListener("pointerup", up);
      over.removeEventListener("pointercancel", up);
      if (registry?.plots.get(chartKey) === plot) registry.plots.delete(chartKey);
      plot.destroy();
    };
    // `settingsKey` stands for `settings`; the data and labels arrays are memoized by the caller.
  }, [chartKey, data, labels, settingsKey, theme, empty, registry]);
  if (empty) return <p>No metric points yet.</p>;
  return <div ref={target} className="experiment-chart" role="img" aria-label={`${settings.title || chartKey}. Data table follows.`} />;
}

/** The toolbar's zoom and pan over every metric chart (±25%, reset to the data). */
export function viewAction(registry: ChartRegistry, action: "in" | "out" | "left" | "right" | "reset") {
  for (const [key, plot] of registry.plots) {
    const min = plot.scales.x.min ?? 0;
    const max = plot.scales.x.max ?? 0;
    const span = max - min;
    if (action === "reset") {
      const xs = plot.data[0];
      plot.setScale("x", { min: xs[0], max: xs[xs.length - 1] });
      registry.views.delete(key);
    } else if (action === "left" || action === "right") {
      const offset = span * (action === "left" ? -0.25 : 0.25);
      plot.setScale("x", { min: min + offset, max: max + offset });
    } else {
      const offset = span * (action === "in" ? 0.25 : -0.5);
      plot.setScale("x", { min: min + offset, max: max - offset });
    }
  }
}
