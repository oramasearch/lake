# Experiment Chart Assets

uPlot 1.6.32 is vendored unchanged from the upstream tag:

- `uPlot.iife.js`: https://github.com/leeoniya/uPlot/blob/1.6.32/dist/uPlot.iife.js
- `uPlot.min.css`: https://github.com/leeoniya/uPlot/blob/1.6.32/dist/uPlot.min.css
- `LICENSE.uPlot`: https://github.com/leeoniya/uPlot/blob/1.6.32/LICENSE (MIT)

SHA-256:

```text
1b71fc5e6b5b572922ed9941ed21d067207c8e5ecac0d35de66fd65d9686e791  uPlot.iife.js
0cf09be05fa0760ca9a3330ea374f6655f08766c7b2b370e462afe98852147ce  uPlot.min.css
3421f0033bae76860e165efab33d4bbbcc2d8fc1a4a708ef4f6742eef434ab47  LICENSE.uPlot
```

`experiments.js` is Lake's capability client (its errors prefix Zetta's field `path`), accessible chart controls, transformations and live
poll controller. `experiments.css` consumes Lake's design tokens. CMake embeds all four served
files and their content hashes; no network dependency or runtime asset directory is needed.

When updating the library, replace the two distribution files and license together, record the
tag and hashes here, rebuild, and repeat `Experiments.*`, `lake-experiment-charts`, token gates and
the browser checks in [the UI contract](../../docs/ui/experiments.md). The matching
[API contract](../../docs/api/experiments.md) documents the data and action permissions.
