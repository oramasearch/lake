/* Training experiment charts and controls. All data is read through Lake capabilities. */
(() => {
  'use strict';

  function ema(points, weight) {
    let previous;
    return points.map(point => {
      const value = previous === undefined ? point.value :
        weight * previous + (1 - weight) * point.value;
      previous = value;
      return {...point, value};
    });
  }

  function metricData(details, name, axis, weight, logarithmic) {
    const columns = details.map(run => {
      if (run.metrics_suppressed) {
        return new Map();
      }
      const times = run.series.flatMap(series => series.points.map(point => Date.parse(point.time)))
        .filter(Number.isFinite);
      const start = times.length ? times.reduce((a, b) => Math.min(a, b)) : 0;
      const points = [...(run.series.find(series => series.name === name)?.points || [])]
        .sort((a, b) => a.step - b.step);
      return new Map(ema(points, weight).map(point => {
        const x = axis === 'step' ? point.step :
          (Date.parse(point.time) - (axis === 'relative' ? start : 0)) / 1000;
        const value = logarithmic && point.value <= 0 ? null : point.value;
        return [x, value];
      }).filter(([x]) => Number.isFinite(x)));
    });
    const x = [...new Set(columns.flatMap(column => [...column.keys()]))].sort((a, b) => a - b);
    return [x, ...columns.map(column => x.map(at => column.has(at) ? column.get(at) : null))];
  }

  function parameterDiff(details) {
    const names = [...new Set(details.flatMap(run => Object.keys(run.params)))].sort();
    return names.map(name => {
      const values = details.map(run => Object.hasOwn(run.params, name) ?
        JSON.stringify(run.params[name]) : '—');
      return {name, values, differs: new Set(values).size > 1};
    });
  }

  function nextCursor(batch, previous) {
    // A full batch must continue after its last event, never skip to the allocator high-water mark.
    if (batch.events.length) {
      return Math.max(previous, ...batch.events.map(event => event.sequence + 1));
    }
    return Math.max(previous, batch.next_sequence);
  }

  if (typeof module !== 'undefined') {
    module.exports = {ema, metricData, parameterDiff, nextCursor};
  }
  if (typeof document === 'undefined') {
    return;
  }
  const root = document.getElementById('experiments');
  if (!root) {
    return;
  }
  const state = JSON.parse(document.getElementById('experiment-data').textContent);
  const notice = document.getElementById('experiment-notice');
  const live = document.getElementById('experiment-live');
  const charts = new Map();
  const views = new Map();
  const cursors = new Map();
  const columns = new Map();
  const trialStates = new Map();
  let details = state.details;
  let trialMetrics = [];
  let trials = state.data.trials || [];
  let sortKey = 'number';
  let descending = false;
  let pending = false;
  let stopped = false;
  let refreshAgain = false;

  function node(tag, text, attributes = {}) {
    const element = document.createElement(tag);
    if (text !== undefined && text !== null) {
      element.textContent = String(text);
    }
    for (const [key, value] of Object.entries(attributes)) {
      element.setAttribute(key, String(value));
    }
    return element;
  }

  function control(tag, text, capability, attributes = {}) {
    return node(tag, text, {'data-capability': capability, ...attributes});
  }

  function table(headers, rows, caption) {
    const wrapper = node('div', null, {class: 'table-scroll'});
    const result = node('table');
    if (caption) {
      result.append(node('caption', caption));
    }
    const head = node('tr');
    headers.forEach(label => head.append(node('th', label, {scope: 'col'})));
    const body = node('tbody');
    rows.forEach(values => {
      const row = node('tr');
      values.forEach(value => row.append(node('td', value ?? '—')));
      body.append(row);
    });
    const thead = node('thead');
    thead.append(head);
    result.append(thead, body);
    wrapper.append(result);
    return wrapper;
  }

  async function call(capability, input) {
    const response = await fetch('/v1/' + capability, {
      method: 'POST', credentials: 'same-origin',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({customer_id: state.customer_id, ...input})
    });
    const result = await response.json();
    if (!response.ok || !result.ok) {
      throw new Error(result.error?.message || 'The request failed.');
    }
    return result.value;
  }

  function report(error) {
    notice.textContent = error.message;
  }

  function url(path, input) {
    return path + '?' + new URLSearchParams({customer_id: state.customer_id, ...input});
  }

  function confirmation(capability, input, message) {
    const dialog = document.getElementById('experiment-confirm');
    const accept = document.getElementById('confirm-accept');
    const cancel = document.getElementById('confirm-cancel');
    document.getElementById('confirm-message').textContent = message;
    document.getElementById('confirm-details').textContent = JSON.stringify(input, null, 2);
    accept.dataset.capability = capability;
    cancel.dataset.capability = capability;
    dialog.returnValue = '';
    dialog.showModal();
    return new Promise(resolve => {
      accept.onclick = () => dialog.close('confirm');
      cancel.onclick = () => dialog.close('cancel');
      dialog.addEventListener('close', () => resolve(dialog.returnValue === 'confirm'), {once: true});
    });
  }

  async function performAction(button) {
    const capability = button.dataset.action;
    const input = {study: button.dataset.study};
    if (button.dataset.run) {
      input.run = button.dataset.run;
    }
    const message = capability === 'promote_trial' ?
      'Promote this completed trial’s registered model as the study’s project model version?' :
      'Stop this training work? Active trials will be ended cooperatively.';
    if (!await confirmation(capability, input, message)) {
      return;
    }
    button.disabled = true;
    try {
      const result = await call(capability, {...input, confirm: true});
      notice.textContent = capability === 'promote_trial' ? 'Promoted model ' + result.model :
        'Training stop requested.';
      if (state.mode === 'list') {
        location.reload();
      } else {
        await refresh();
      }
    } finally {
      button.disabled = false;
    }
  }

  root.querySelectorAll('[data-action], [data-start]').forEach(button => {
    button.disabled = false;
  });
  root.addEventListener('click', event => {
    const button = event.target.closest('[data-action]');
    if (button) {
      performAction(button).catch(report);
    }
  });

  document.getElementById('start-study')?.addEventListener('submit', async event => {
    event.preventDefault();
    const form = event.currentTarget;
    const input = {};
    try {
      for (const element of form.elements) {
        if (!element.name || element.name === 'search_space' || element.name === 'released_metrics') {
          continue;
        }
        const value = element.type === 'checkbox' ? element.checked : element.type === 'number' ?
          Number(element.value) : element.value.trim();
        if (value === '') {
          continue;
        }
        const [parent, child] = element.name.split('.');
        if (child) {
          input[parent] ||= {};
          input[parent][child] = value;
        } else {
          input[parent] = value;
        }
      }
      const search = form.elements.search_space.value.trim();
      if (search) {
        input.search_space = JSON.parse(search);
      } else {
        input.from_defaults = true;
      }
      input.released_metrics = form.elements.released_metrics.value.split(',')
        .map(value => value.trim()).filter(Boolean);
      if (!await confirmation('start_study', input, 'Start training with these parameters and budgets?')) {
        return;
      }
      form.querySelector('[data-start]').disabled = true;
      const study = await call('start_study', {...input, confirm: true});
      location.href = url('/train/experiment', {study: study.id});
    } catch (error) {
      report(error);
      form.querySelector('[data-start]').disabled = false;
    }
  });

  document.getElementById('compare-selection')?.addEventListener('submit', event => {
    event.preventDefault();
    const runs = [...event.currentTarget.querySelectorAll('input[name="runs"]:checked')]
      .map(input => input.value);
    if (runs.length < 2 || runs.length > 8) {
      report(new Error('Select two to eight runs to compare.'));
      return;
    }
    location.href = url('/train/experiments/compare', {runs: runs.join(',')});
  });

  function token(name) {
    // Canvas needs resolved colors; computed custom properties may still contain var().
    const sample = node('span', null);
    sample.style.color = 'var(' + name + ')';
    root.append(sample);
    const color = getComputedStyle(sample).color;
    sample.remove();
    return color;
  }

  function palette() {
    return ['--lake-action', '--lake-ai', '--lake-success', '--lake-warning',
      '--lake-danger', '--lake-info', '--lake-text-primary', '--lake-text-secondary'].map(token);
  }

  function destroy(key) {
    const chart = charts.get(key);
    if (chart) {
      chart.observer.disconnect();
      chart.plot.destroy();
      charts.delete(key);
    }
  }

  function createChart(key, target, data, labels, settings = {}) {
    destroy(key);
    if (!data[0].length || !labels.length) {
      target.replaceChildren(node('p', 'No metric points yet.'));
      return;
    }
    target.replaceChildren();
    const colors = palette();
    const text = token('--lake-text-secondary');
    const border = token('--lake-border-subtle');
    let panning = null;
    let ready = false;
    const options = {
      width: Math.max(260, target.clientWidth), height: 280,
      series: [{label: settings.xLabel || 'Step'}, ...labels.map((label, index) => ({
        label, stroke: colors[index % colors.length], width: 2,
        dash: index >= 6 ? [6, 3] : [], spanGaps: false,
        points: {show: data[0].length < 50, size: 5}
      }))],
      scales: {x: {time: settings.time || false}, y: {distr: settings.log ? 3 : 1}},
      axes: [{stroke: text, grid: {stroke: border}, ticks: {stroke: border}},
        {stroke: text, grid: {stroke: border}, ticks: {stroke: border}}],
      cursor: {drag: {x: true, y: false}, bind: {
        mousedown: (plot, targetElement, handler) => event => {
          if (!event.shiftKey) {
            handler(event);
          }
        }
      }},
      hooks: {setScale: [(plot, scale) => {
        if (ready && scale === 'x' && settings.metric) {
          views.set(key, {min: plot.scales.x.min, max: plot.scales.x.max});
        }
      }]}
    };
    if (settings.xValues) {
      options.axes[0].values = (_, ticks) => ticks.map(value => settings.xValues[value] || '');
      options.axes[0].incrs = [1];
      options.series[0].value = (_, value) => settings.xValues[value] || value;
    }
    const saved = views.get(key);
    const plot = new uPlot(options, data, target);
    if (saved && settings.metric) {
      plot.setScale('x', saved);
    }
    ready = true;
    plot.over.addEventListener('pointerdown', event => {
      if (event.shiftKey) {
        panning = {x: event.clientX, min: plot.scales.x.min, max: plot.scales.x.max};
        plot.over.setPointerCapture(event.pointerId);
        event.preventDefault();
      }
    });
    plot.over.addEventListener('pointermove', event => {
      if (panning) {
        const offset = (event.clientX - panning.x) / plot.over.clientWidth *
          (panning.max - panning.min);
        plot.setScale('x', {min: panning.min - offset, max: panning.max - offset});
      }
    });
    plot.over.addEventListener('pointerup', () => {
      panning = null;
    });
    plot.over.addEventListener('pointercancel', () => {
      panning = null;
    });
    const observer = new ResizeObserver(() => {
      const width = Math.max(260, target.clientWidth);
      if (width !== plot.width) {
        plot.setSize({width, height: 280});
      }
    });
    observer.observe(target);
    charts.set(key, {plot, observer, metric: Boolean(settings.metric)});
    target.setAttribute('role', 'img');
    target.setAttribute('aria-label', (settings.title || key) + '. Data table follows.');
  }

  function drawMetrics() {
    const panels = document.getElementById('metric-panels');
    if (!panels) {
      return;
    }
    for (const [key, chart] of charts) {
      if (chart.metric) {
        destroy(key);
      }
    }
    const expanded = new Set([...panels.querySelectorAll('details[open]')]
      .map(item => item.dataset.metric));
    panels.replaceChildren();
    const names = [...new Set(details.filter(run => !run.metrics_suppressed)
      .flatMap(run => run.series.map(series => series.name)))].sort();
    const axis = document.getElementById('chart-axis').value;
    const weight = Number(document.getElementById('chart-smoothing').value) / 100;
    const log = document.getElementById('chart-log').checked;
    document.getElementById('smoothing-value').textContent = Math.round(weight * 100) + '%';
    const groups = new Map();
    for (const run of details.filter(run => run.metrics_suppressed)) {
      panels.append(node('p', run.run + ': curves suppressed for differential privacy'));
    }
    if (!names.length) {
      panels.append(node('p', 'No training curves have been recorded. Live updates will appear here.'));
    }
    for (const name of names) {
      const prefix = name.includes('.') ? name.split('.')[0] : 'Metrics';
      if (!groups.has(prefix)) {
        const group = node('section', null, {class: 'panel'});
        group.append(node('h2', prefix === 'Metrics' ? prefix :
          prefix[0].toUpperCase() + prefix.slice(1) + ' Metrics'));
        groups.set(prefix, group);
        panels.append(group);
      }
      const article = node('article', null, {class: 'experiment-plot'});
      article.append(node('h3', name));
      const plot = node('div', null, {class: 'experiment-chart'});
      article.append(plot);
      groups.get(prefix).append(article);
      const data = metricData(details, name, axis, weight, log);
      createChart('metric:' + name, plot, data, details.map(run => run.run), {
        title: name, time: axis === 'wall', log, metric: true,
        xLabel: axis === 'step' ? 'Step' : axis === 'wall' ? 'Wall time' : 'Seconds since run start'
      });
      const disclosure = node('details', null, {'data-metric': name});
      disclosure.open = expanded.has(name);
      disclosure.append(control('summary', 'Chart Data', 'get_run_metrics'));
      const rows = details.flatMap(run => {
        if (run.metrics_suppressed) {
          return [];
        }
        const points = run.series.find(series => series.name === name)?.points || [];
        const smoothed = ema([...points].sort((a, b) => a.step - b.step), weight);
        const original = new Map(points.map(point => [point.step, point.value]));
        return smoothed.map(point => [run.run, point.step, point.time,
          original.get(point.step), point.value]);
      });
      disclosure.append(table(['Run', 'Step', 'Wall time', 'Raw value', 'Smoothed value'], rows, name));
      article.append(disclosure);
    }
    const diff = document.getElementById('params-diff');
    const differences = parameterDiff(details);
    diff.replaceChildren(table(['Parameter', 'Differs', ...details.map(run => run.run)],
      differences.map(row => [row.name, row.differs ? 'Yes' : 'No', ...row.values])));
    document.getElementById('initial-data')?.setAttribute('hidden', '');
    drawArtifacts();
  }

  function drawArtifacts() {
    const target = document.getElementById('run-artifacts');
    if (!target) {
      return;
    }
    for (const key of charts.keys()) {
      if (key.startsWith('artifact:')) {
        destroy(key);
      }
    }
    target.replaceChildren();
    let count = 0;
    for (const run of details) {
      for (const artifact of run.artifacts) {
        const key = 'artifact:' + count++;
        const section = node('section', null, {class: 'experiment-artifact'});
        section.append(node('h3', artifact.name + ' · ' + run.run));
        const data = artifact.data;
        if (artifact.type === 'confusion_matrix') {
          section.append(table(['Actual / Predicted', ...data.labels],
            data.matrix.map((row, index) => [data.labels[index], ...row]), 'Confusion Matrix'));
        } else if (artifact.type === 'sample_predictions') {
          const keys = [...new Set(data.rows.flatMap(row => Object.keys(row)))];
          section.append(table(keys, data.rows.map(row => keys.map(key => row[key])),
            'Sample Predictions'));
        } else {
          const plot = node('div', null, {class: 'experiment-chart'});
          section.append(plot);
          target.append(section);
          if (artifact.type === 'feature_importance') {
            createChart(key, plot, [data.values.map((_, index) => index), data.values],
              ['Importance'], {xValues: data.features});
            section.append(table(['Feature', 'Importance'],
              data.features.map((feature, index) => [feature, data.values[index]])));
          } else if (artifact.type === 'calibration') {
            createChart(key, plot, [data.predicted, data.observed, data.predicted],
              ['Observed', 'Ideal']);
            section.append(table(['Predicted', 'Observed', 'Count'],
              data.predicted.map((value, index) => [value, data.observed[index], data.counts[index]])));
          } else {
            createChart(key, plot, [data.x, data.y],
              [artifact.type === 'roc_curve' ? 'True positive rate' : 'Precision']);
            section.append(table(['X', 'Y', 'Threshold'],
              data.x.map((value, index) => [value, data.y[index], data.thresholds[index]])));
          }
        }
        target.append(section);
      }
    }
    if (!count) {
      target.append(node('p', 'No artifacts have been recorded.'));
    }
  }

  function drawStudy() {
    const analysis = state.analysis;
    const history = analysis.optimization_history || [];
    createChart('history', document.getElementById('optimization-chart'),
      [history.map(row => row.number), history.map(row => row.value), history.map(row => row.best)],
      ['Objective', 'Best so far'], {title: 'Optimization History'});
    document.getElementById('optimization-table').replaceChildren(table(
      ['Trial', 'Run', 'Objective', 'Best so far'],
      history.map(row => [row.number, row.run, row.value, row.best])));
    const rows = analysis.parallel_coordinates || [];
    const parameters = [...new Set(rows.flatMap(row => Object.keys(row.params)))].sort();
    const labels = [...parameters, 'Objective'];
    const raw = rows.map(row => [...parameters.map(key => row.params[key]), row.value]);
    const normalized = labels.map((_, index) => {
      const values = raw.map(row => row[index]);
      const numeric = values.every(value => value === undefined || typeof value === 'number');
      const categories = [...new Set(values.filter(value => value !== undefined))];
      const numbers = numeric ? values : values.map(value => categories.indexOf(value));
      const finite = numbers.filter(Number.isFinite);
      const min = finite.length ? Math.min(...finite) : 0;
      const max = finite.length ? Math.max(...finite) : 1;
      return numbers.map((value, row) => values[row] === undefined ? null :
        max === min ? 0.5 : (value - min) / (max - min));
    });
    createChart('parallel', document.getElementById('parallel-chart'),
      [labels.map((_, index) => index), ...rows.map((_, row) => normalized.map(column => column[row]))],
      rows.map(row => row.run), {xValues: labels, title: 'Parallel Coordinates'});
    document.getElementById('parallel-table').replaceChildren(node('p',
      'Axes are normalized from 0 to 1; categorical values follow first occurrence. Exact values:'),
      table(['Run', ...labels], rows.map((row, index) => [row.run, ...raw[index]])));
    const importance = analysis.parameter_importances || [];
    createChart('importance', document.getElementById('importance-chart'),
      [importance.map((_, index) => index), importance.map(row => row.importance)], ['Importance'],
      {xValues: importance.map(row => row.parameter), title: 'Parameter Importance'});
    document.getElementById('importance-table').replaceChildren(table(['Parameter', 'Importance'],
      importance.map(row => [row.parameter, row.importance])));
    const budgets = state.data.spec.budgets;
    document.getElementById('study-status').textContent = 'State: ' + state.data.status +
      ' · Trials: ' + state.data.trials.length + ' / ' + budgets.max_trials +
      ' · Cost: ' + state.data.spent_cost + ' / ' + budgets.cost_cap +
      ' · Objective: ' + state.data.spec.objective.metric + ' (' +
      state.data.spec.objective.direction + ')' +
      (state.data.stop_reason ? ' · ' + state.data.stop_reason : '') +
      (state.data.promoted_model ? ' · Promoted model: ' + state.data.promoted_model : '');
    drawLeaderboard();
  }

  function trialValue(trial, key) {
    if (key.startsWith('param:')) {
      return trial.params[key.slice(6)];
    }
    if (key.startsWith('metric:')) {
      return trialMetrics.find(run => run.run === trial.run)?.summary[key.slice(7)]?.final;
    }
    if (key === 'confidence') {
      const row = state.analysis.leaderboard?.find(row => row.run === trial.run);
      return row ? row.confidence_low + ' – ' + row.confidence_high : undefined;
    }
    return trial[key];
  }

  function drawLeaderboard() {
    const available = ['number', 'state', 'objective', 'cost', 'confidence',
      ...new Set(state.data.trials.flatMap(trial => Object.keys(trial.params).map(key => 'param:' + key))),
      ...new Set(trialMetrics.flatMap(run => Object.keys(run.summary).map(key => 'metric:' + key)))];
    const picker = document.getElementById('trial-columns');
    const current = new Set([...picker.querySelectorAll('input')].map(input => input.value));
    for (const key of available) {
      if (!columns.has(key)) {
        columns.set(key, true);
      }
      if (!current.has(key)) {
        const label = node('label');
        const checkbox = control('input', null, 'get_experiment_analysis', {type: 'checkbox', value: key});
        checkbox.checked = columns.get(key);
        checkbox.addEventListener('change', () => {
          columns.set(key, checkbox.checked);
          drawLeaderboard();
        });
        label.append(checkbox, document.createTextNode(key.replace(/^(param|metric):/, '')));
        picker.append(label);
      }
    }
    const visible = available.filter(key => columns.get(key));
    const selected = new Set([...document.querySelectorAll('#leaderboard input:checked')]
      .map(input => input.value));
    const result = node('table');
    const heading = node('tr');
    heading.append(node('th', 'Compare'), node('th', 'Run'));
    for (const key of visible) {
      const cell = node('th', null, {scope: 'col', 'aria-sort': sortKey === key ?
        descending ? 'descending' : 'ascending' : 'none'});
      const button = control('button', key.replace(/^(param|metric):/, ''),
        'get_experiment_analysis', {type: 'button'});
      button.onclick = () => {
        descending = sortKey === key ? !descending : false;
        sortKey = key;
        drawLeaderboard();
      };
      cell.append(button);
      heading.append(cell);
    }
    heading.append(node('th', 'Actions'));
    const head = node('thead');
    head.append(heading);
    const body = node('tbody');
    const sorted = [...trials].sort((a, b) => {
      const x = trialValue(a, sortKey);
      const y = trialValue(b, sortKey);
      const order = typeof x === 'number' && typeof y === 'number' ? x - y :
        String(x ?? '').localeCompare(String(y ?? ''));
      return descending ? -order : order;
    });
    for (const trial of sorted) {
      const row = node('tr');
      const select = node('td');
      const checkbox = control('input', null, 'compare_runs', {type: 'checkbox', name: 'runs',
        value: trial.run, 'aria-label': 'Compare trial ' + trial.number});
      checkbox.checked = selected.has(trial.run);
      select.append(checkbox);
      const runCell = node('td');
      runCell.append(control('a', trial.run, 'get_run_metrics',
        {href: url('/train/experiment/run', {run: trial.run})}));
      row.append(select, runCell);
      visible.forEach(key => row.append(node('td', trialValue(trial, key) ?? '—')));
      const actions = node('td');
      let capability;
      if (trial.state === 'running' && state.can_write) {
        capability = 'stop_experiment_trial';
      } else if (trial.state === 'complete' && trial.model && state.can_promote) {
        capability = 'promote_trial';
      }
      if (capability) {
        actions.append(control('button', capability === 'promote_trial' ? 'Promote' : 'Stop trial',
          capability, {type: 'button', 'data-action': capability,
            'data-study': state.study, 'data-run': trial.run}));
      }
      row.append(actions);
      body.append(row);
    }
    result.append(head, body);
    document.getElementById('leaderboard').replaceChildren(result);
  }

  async function refresh() {
    if (pending) {
      refreshAgain = true;
      return;
    }
    pending = true;
    try {
      if (state.mode === 'study') {
        const filter = document.getElementById('trial-state').value;
        const responses = await Promise.all([
          call('get_experiment_study', {study: state.study}),
          call('get_experiment_analysis', {study: state.study}),
          call('list_experiment_trials', {study: state.study, ...(filter ? {state: filter} : {})})
        ]);
        state.data = responses[0];
        state.analysis = responses[1];
        trials = responses[2].trials;
        const queue = [...state.data.trials];
        const refreshed = new Map();
        await Promise.all(Array.from({length: Math.min(4, queue.length)}, async () => {
          while (queue.length) {
            const trial = queue.shift();
            const cached = trialMetrics.find(run => run.run === trial.run);
            const unchanged = trial.state !== 'running' && trialStates.get(trial.run) === trial.state;
            refreshed.set(trial.run, unchanged && cached ? cached :
              await call('get_run_metrics', {run: trial.run}));
            trialStates.set(trial.run, trial.state);
          }
        }));
        trialMetrics = state.data.trials.map(trial => refreshed.get(trial.run));
        details = trialMetrics.slice(-8);
        drawStudy();
      } else if (state.mode === 'compare') {
        state.data = await call('compare_runs', {runs: state.runs});
        details = state.data.details;
      } else if (state.mode === 'run') {
        details = [await call('get_run_metrics', {run: state.run})];
      }
      drawMetrics();
    } finally {
      pending = false;
      if (refreshAgain) {
        refreshAgain = false;
        refresh().catch(report);
      }
    }
  }

  function targets() {
    const result = state.mode === 'study' ? [{kind: 'study', id: state.study}] : [];
    for (const run of details) {
      if (!run.metrics_suppressed) {
        result.push({kind: 'run', id: run.run});
      }
    }
    return result;
  }

  async function follow() {
    const status = document.getElementById('live-status');
    while (!stopped) {
      const began = Date.now();
      if (!live.checked || document.hidden) {
        status.textContent = 'Paused';
        await new Promise(resolve => setTimeout(resolve, 1000));
        continue;
      }
      try {
        const polls = targets();
        await Promise.all(polls.map(async target => {
          const key = target.kind + ':' + target.id;
          const previous = cursors.get(key) || 1;
          const batch = await call('tail_experiment_events', {...target,
            from_sequence: previous, max_events: 100, wait_ms: 10000});
          cursors.set(key, nextCursor(batch, previous));
          if (batch.events.length) {
            await refresh();
          }
        }));
        // Refresh also repairs gaps when an ephemeral topic is absent or has expired.
        if (live.checked && !stopped) {
          await refresh();
          status.textContent = 'Live · updated ' + new Date().toLocaleTimeString();
        }
      } catch (error) {
        status.textContent = 'Reconnecting · ' + error.message;
      }
      await new Promise(resolve => setTimeout(resolve, Math.max(1000, 10000 - (Date.now() - began))));
    }
  }

  document.getElementById('trial-state')?.addEventListener('change', () => refresh().catch(report));
  document.getElementById('chart-axis')?.addEventListener('change', () => {
    views.clear();
    drawMetrics();
  });
  document.getElementById('chart-log')?.addEventListener('change', drawMetrics);
  document.getElementById('chart-smoothing')?.addEventListener('input', drawMetrics);
  root.querySelectorAll('[data-view]').forEach(button => {
    button.addEventListener('click', () => {
      const action = button.dataset.view;
      for (const [key, chart] of charts) {
        if (!chart.metric) {
          continue;
        }
        const plot = chart.plot;
        const {min, max} = plot.scales.x;
        const span = max - min;
        if (action === 'reset') {
          plot.setScale('x', {min: plot.data[0][0], max: plot.data[0].at(-1)});
          views.delete(key);
        } else if (action === 'left' || action === 'right') {
          const offset = span * (action === 'left' ? -0.25 : 0.25);
          plot.setScale('x', {min: min + offset, max: max + offset});
        } else {
          const offset = span * (action === 'in' ? 0.25 : -0.5);
          plot.setScale('x', {min: min + offset, max: max - offset});
        }
      }
    });
  });
  const themeObserver = new MutationObserver(() => {
    drawMetrics();
    if (state.mode === 'study' && state.data.spec) {
      drawStudy();
    }
  });
  themeObserver.observe(document.documentElement, {attributes: true, attributeFilter: ['data-theme']});
  matchMedia('(prefers-color-scheme: dark)').addEventListener('change', () => {
    drawMetrics();
    if (state.mode === 'study' && state.data.spec) {
      drawStudy();
    }
  });
  addEventListener('pagehide', () => {
    stopped = true;
    themeObserver.disconnect();
    [...charts.keys()].forEach(destroy);
  });
  if (state.mode !== 'list') {
    drawMetrics();
    refresh().catch(report).then(follow);
  }
})();
