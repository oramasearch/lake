R"lake_styles(
@font-face {
  font-family: 'IBM Plex Sans';
  font-style: normal;
  font-weight: 400;
  font-display: swap;
  src: url('/fonts/ibm-plex-sans-400.woff2') format('woff2');
}
@font-face {
  font-family: 'IBM Plex Sans';
  font-style: normal;
  font-weight: 500;
  font-display: swap;
  src: url('/fonts/ibm-plex-sans-500.woff2') format('woff2');
}
@font-face {
  font-family: 'IBM Plex Sans';
  font-style: normal;
  font-weight: 600;
  font-display: swap;
  src: url('/fonts/ibm-plex-sans-600.woff2') format('woff2');
}
@font-face {
  font-family: 'IBM Plex Mono';
  font-style: normal;
  font-weight: 400;
  font-display: swap;
  src: url('/fonts/ibm-plex-mono-400.woff2') format('woff2');
}
@font-face {
  font-family: 'IBM Plex Mono';
  font-style: normal;
  font-weight: 500;
  font-display: swap;
  src: url('/fonts/ibm-plex-mono-500.woff2') format('woff2');
}

* {
  box-sizing: border-box;
}
html {
  background: var(--lake-canvas);
  scroll-behavior: smooth;
}
body {
  min-width: 20rem;
  margin: 0;
  color: var(--lake-text-primary);
  background: var(--lake-canvas);
  font-family: var(--lake-font-sans);
  font-size: var(--lake-text-14);
  font-weight: var(--lake-weight-regular);
  line-height: var(--lake-leading-copy);
}
a {
  color: var(--lake-action);
  text-decoration-thickness: var(--lake-border-width);
  text-underline-offset: 0.16em;
}
a:hover {
  color: var(--lake-action-hover);
}
:where(a, button, input, textarea, select, [tabindex]):focus-visible {
  outline: 0.125rem solid var(--lake-focus);
  outline-offset: 0.125rem;
}
::selection {
  color: var(--lake-text-primary);
  background: var(--lake-selection);
}
.sr-only {
  position: absolute;
  width: 1px;
  height: 1px;
  overflow: hidden;
  clip: rect(0, 0, 0, 0);
  white-space: nowrap;
  clip-path: inset(50%);
}

.hosted-shell {
  min-height: 100vh;
  background: color-mix(in srgb, var(--app-background) 8%, var(--lake-canvas));
}
.hosted-header {
  min-height: var(--lake-topbar-height);
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--lake-space-4);
  padding: 0 var(--lake-page-gutter);
  border-bottom: var(--lake-border-width) solid var(--lake-border-subtle);
  background: var(--lake-surface);
}
.hosted-brand {
  display: flex;
  align-items: center;
  gap: var(--lake-space-2);
  font-size: var(--lake-text-16);
  font-weight: var(--lake-weight-semibold);
}
.hosted-brand-mark {
  width: 0.25rem;
  height: 1.25rem;
  background: var(--app-accent);
}
.hosted-main {
  width: min(100%, 34rem);
  margin: 0 auto;
  padding: var(--lake-space-8) var(--lake-page-gutter) 4rem;
}
.hosted-card {
  padding: var(--lake-space-6);
  border: var(--lake-border-width) solid var(--lake-border-subtle);
  border-top: 0.1875rem solid var(--app-accent);
  background: var(--lake-surface);
}
.hosted-card form + p,
.hosted-card p + form {
  margin-top: var(--lake-space-6);
}

.skip-link {
  position: fixed;
  z-index: calc(var(--lake-z-toast) + 1);
  top: var(--lake-space-2);
  left: var(--lake-space-2);
  padding: var(--lake-space-2) var(--lake-space-3);
  color: var(--lake-text-on-action);
  background: var(--lake-action-filled);
  transform: translateY(-200%);
}
.skip-link:focus {
  transform: translateY(0);
}
.app-shell {
  min-height: 100vh;
  display: grid;
  grid-template-columns: var(--lake-sidebar-width) minmax(0, 1fr);
  /* The sidebar is sticky and one viewport tall; its column is painted here so the surface and
     its border run the full height of a long page. */
  background: linear-gradient(
    to right,
    var(--lake-surface) calc(var(--lake-sidebar-width) - var(--lake-border-width)),
    var(--lake-border-subtle) 0 var(--lake-sidebar-width),
    transparent 0
  );
}
.sidebar {
  position: sticky;
  z-index: var(--lake-z-sticky);
  top: 0;
  height: 100vh;
  display: flex;
  flex-direction: column;
  border-right: var(--lake-border-width) solid var(--lake-border-subtle);
  background: var(--lake-surface);
}
.brand {
  min-height: var(--lake-topbar-height);
  display: flex;
  align-items: center;
  gap: var(--lake-space-2);
  padding: 0 var(--lake-space-4);
  border-bottom: var(--lake-border-width) solid var(--lake-border-subtle);
  color: var(--lake-text-primary);
  font-size: var(--lake-text-16);
  font-weight: var(--lake-weight-semibold);
  text-decoration: none;
}
.brand-mark {
  width: 1.25rem;
  height: 1.25rem;
  display: grid;
  place-items: center;
  color: var(--lake-text-on-action);
  background: var(--lake-action-filled);
}
.primary-nav {
  min-height: 0;
  flex: 1;
  overflow-y: auto;
  padding: var(--lake-space-3) var(--lake-space-2);
}
.nav-label {
  margin: var(--lake-space-4) var(--lake-space-2) var(--lake-space-1);
  color: var(--lake-text-tertiary);
  font-size: var(--lake-text-11);
  font-weight: var(--lake-weight-semibold);
  letter-spacing: 0.08em;
  text-transform: uppercase;
}
.nav-label:first-child {
  margin-top: 0;
}
.nav-link {
  min-height: var(--lake-control-sm);
  display: flex;
  align-items: center;
  gap: var(--lake-space-3);
  padding: var(--lake-space-1) var(--lake-space-2);
  border-left: 0.125rem solid transparent;
  color: var(--lake-text-secondary);
  line-height: var(--lake-leading-ui);
  text-decoration: none;
  transition: color var(--lake-motion-fast) var(--lake-motion-ease),
    background var(--lake-motion-fast) var(--lake-motion-ease);
}
.nav-link:hover {
  color: var(--lake-text-primary);
  background: var(--lake-layer-hover);
}
.nav-link[aria-current='page'] {
  border-left-color: var(--lake-action);
  color: var(--lake-text-primary);
  background: var(--lake-selected);
  font-weight: var(--lake-weight-medium);
}
.icon {
  width: 1rem;
  height: 1rem;
  flex: 0 0 auto;
  fill: currentColor;
}
.sidebar-footer {
  padding: var(--lake-space-3) var(--lake-space-4);
  border-top: var(--lake-border-width) solid var(--lake-border-subtle);
  color: var(--lake-text-tertiary);
  font-size: var(--lake-text-12);
}
.app-frame {
  min-width: 0;
}
.topbar {
  position: sticky;
  z-index: var(--lake-z-sticky);
  top: 0;
  min-height: var(--lake-topbar-height);
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--lake-space-4);
  padding: 0 var(--lake-page-gutter);
  border-bottom: var(--lake-border-width) solid var(--lake-border-subtle);
  background: var(--lake-surface);
}
.topbar-context {
  min-width: 0;
  overflow: hidden;
  color: var(--lake-text-secondary);
  font-size: var(--lake-text-12);
  text-overflow: ellipsis;
  white-space: nowrap;
}
.topbar-actions,
.session {
  display: flex;
  align-items: center;
  gap: var(--lake-space-3);
  white-space: nowrap;
}
.session {
  color: var(--lake-text-secondary);
  font-size: var(--lake-text-12);
}
.account-menu {
  position: relative;
  font-size: var(--lake-text-12);
}
.account-menu summary {
  display: flex;
  align-items: center;
  gap: var(--lake-space-2);
  min-height: var(--lake-control-sm);
  padding: 0 var(--lake-space-3);
  border: var(--lake-border-width) solid var(--lake-border-subtle);
  border-radius: var(--lake-radius-control);
  color: var(--lake-text-primary);
  cursor: pointer;
  list-style: none;
}
.account-menu summary::-webkit-details-marker {
  display: none;
}
.account-menu[open] summary {
  border-color: var(--lake-border-strong);
}
.account-menu-panel {
  position: absolute;
  right: 0;
  top: calc(100% + var(--lake-space-1));
  z-index: 10;
  display: flex;
  flex-direction: column;
  min-width: 11rem;
  padding: var(--lake-space-1) 0;
  border: var(--lake-border-width) solid var(--lake-border-subtle);
  border-radius: var(--lake-radius-control);
  background: var(--lake-layer);
}
.account-menu-panel a,
.account-menu-panel button {
  display: block;
  width: 100%;
  padding: var(--lake-space-2) var(--lake-space-3);
  border: 0;
  background: none;
  color: var(--lake-text-primary);
  font-size: var(--lake-text-12);
  text-align: left;
  text-decoration: none;
}
.account-menu-panel a:hover,
.account-menu-panel button:hover {
  background: var(--lake-surface);
}
.account-menu-panel form {
  border-top: var(--lake-border-width) solid var(--lake-border-subtle);
  margin-top: var(--lake-space-1);
}
.theme-toggle,
.button,
button,
input[type='submit'] {
  min-height: var(--lake-control-sm);
  border: var(--lake-border-width) solid var(--lake-border-interactive);
  border-radius: var(--lake-control-radius);
  padding: 0 var(--lake-space-3);
  color: var(--lake-action);
  background: transparent;
  font: inherit;
  font-weight: var(--lake-weight-medium);
  cursor: pointer;
  transition: color var(--lake-motion-fast) var(--lake-motion-ease),
    background var(--lake-motion-fast) var(--lake-motion-ease);
}
.theme-toggle {
  width: var(--lake-control-sm);
  display: grid;
  place-items: center;
  padding: 0;
  border-color: transparent;
}
.theme-toggle:hover,
.button:hover,
button:hover,
input[type='submit']:hover {
  color: var(--lake-text-on-action);
  background: var(--lake-action-hover);
}
button.primary,
.button-primary,
form.login button,
.filters button,
form.search button {
  border-color: var(--lake-action-filled);
  color: var(--lake-text-on-action);
  background: var(--lake-action-filled);
}
button.primary:hover,
.button-primary:hover,
form.login button:hover,
.filters button:hover,
form.search button:hover {
  border-color: var(--lake-action-filled-hover);
  background: var(--lake-action-filled-hover);
}
button.danger,
.button-danger {
  border-color: var(--lake-danger);
  color: var(--lake-text-on-danger);
  background: var(--lake-danger);
}
button.danger:hover,
.button-danger:hover {
  border-color: var(--lake-danger-hover);
  background: var(--lake-danger-hover);
}
form.inline {
  display: inline;
}
form.inline button {
  border: 0;
  padding: 0 var(--lake-space-1);
}

main {
  min-width: 0;
}
.page-container {
  width: min(100%, 90rem);
  margin: 0 auto;
  padding: var(--lake-space-8) var(--lake-page-gutter) 4rem;
  overflow-x: auto;
}
.eyebrow {
  margin: 0 0 var(--lake-space-1);
  color: var(--lake-text-tertiary);
  font-size: var(--lake-text-11);
  font-weight: var(--lake-weight-semibold);
  letter-spacing: 0.1em;
  text-transform: uppercase;
}
h1,
h2,
h3 {
  color: var(--lake-text-primary);
  font-weight: var(--lake-weight-semibold);
  line-height: var(--lake-leading-tight);
}
h1 {
  margin: 0 0 var(--lake-space-2);
  font-size: var(--lake-text-32);
  letter-spacing: -0.02em;
}
h2 {
  margin: var(--lake-space-8) 0 var(--lake-space-3);
  font-size: var(--lake-text-20);
}
h3 {
  margin: var(--lake-space-6) 0 var(--lake-space-2);
  font-size: var(--lake-text-16);
}
.sub {
  max-width: 72ch;
  margin: 0 0 var(--lake-space-6);
  color: var(--lake-text-secondary);
  font-size: var(--lake-text-16);
}
.panel,
.card {
  padding: var(--lake-space-4);
  border: var(--lake-border-width) solid var(--lake-border-subtle);
  border-radius: var(--lake-panel-radius);
  background: var(--lake-surface);
}
.panel + .panel,
.panel + table,
table + .panel,
.panel + pre {
  margin-top: var(--lake-space-4);
}
.panel > :first-child,
.card > :first-child {
  margin-top: 0;
}
.panel > :last-child,
.card > :last-child {
  margin-bottom: 0;
}
.grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(min(18rem, 100%), 1fr));
  gap: var(--lake-space-4);
}
.drive-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(min(15rem, 100%), 1fr));
  gap: var(--lake-space-3);
}
.drive-card h2 {
  margin: var(--lake-space-2) 0;
  font-size: var(--lake-text-18);
}
.drive-item-actions {
  display: flex;
  align-items: center;
  flex-wrap: wrap;
  gap: var(--lake-space-2);
}
.drive-item-actions details {
  position: relative;
}
.drive-item-actions summary {
  color: var(--lake-action);
  cursor: pointer;
}
.drive-manage {
  min-width: min(22rem, 80vw);
  display: grid;
  gap: var(--lake-space-4);
  margin-top: var(--lake-space-2);
  padding: var(--lake-space-3);
  border: var(--lake-border-width) solid var(--lake-border-subtle);
  background: var(--lake-surface);
}
.drive-drop-zone {
  min-height: 7rem;
  display: grid;
  place-content: center;
  gap: var(--lake-space-2);
  padding: var(--lake-space-4);
  border: 0.125rem dashed var(--lake-border-strong);
  background: var(--lake-layer);
  text-align: center;
}
.drive-drop-zone[data-active='true'] {
  border-color: var(--lake-action);
  background: var(--lake-action-soft);
}
.document-preview,
.media-preview,
.image-preview {
  width: 100%;
  max-height: 70vh;
}
.document-preview {
  min-height: 38rem;
}
.drive-version-diff span {
  display: block;
}
.actions,
.tabs,
.pagination,
.breadcrumbs {
  display: flex;
  align-items: center;
  flex-wrap: wrap;
  gap: var(--lake-space-2);
}
.actions {
  margin: 0 0 var(--lake-space-4);
}
.actions a,
.tabs a,
.pagination a {
  min-height: var(--lake-control-sm);
  display: inline-flex;
  align-items: center;
  padding: 0 var(--lake-space-3);
  border: var(--lake-border-width) solid var(--lake-border-subtle);
  color: var(--lake-text-primary);
  background: var(--lake-surface);
  text-decoration: none;
}
.actions a:hover,
.tabs a:hover,
.pagination a:hover,
.tabs a[aria-current='page'] {
  border-color: var(--lake-border-interactive);
  color: var(--lake-action);
  background: var(--lake-action-soft);
}
.breadcrumbs {
  margin: 0 0 var(--lake-space-3);
  color: var(--lake-text-secondary);
  font-size: var(--lake-text-12);
}
.breadcrumbs a {
  color: inherit;
}
.copy-block {
  margin: var(--lake-space-3) 0;
}
.copy-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--lake-space-2);
}
.copy-head h3 {
  margin: 0;
}
.copy-block pre {
  max-width: 100%;
  overflow: auto;
  white-space: pre;
}
.topbar-trail {
  margin: 0;
}
.topbar-trail ol {
  display: flex;
  align-items: center;
  min-width: 0;
  margin: 0;
  padding: 0;
  list-style: none;
}
.topbar-trail li {
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.topbar-trail li + li::before {
  content: '\203A';
  margin: 0 var(--lake-space-2);
  color: var(--lake-text-secondary);
}
.topbar-trail a {
  color: var(--lake-text-secondary);
  text-decoration: none;
}
.topbar-trail a:hover {
  color: var(--lake-action);
  text-decoration: underline;
}
.topbar-trail [aria-current='page'] {
  color: var(--lake-text-primary);
}

.table-wrap {
  max-width: 100%;
  overflow: auto;
  border: var(--lake-border-width) solid var(--lake-border-subtle);
  background: var(--lake-surface);
}
table {
  width: 100%;
  min-width: 42rem;
  border-spacing: 0;
  border-collapse: separate;
  border: var(--lake-border-width) solid var(--lake-border-subtle);
  background: var(--lake-surface);
  font-variant-numeric: tabular-nums;
}
.table-wrap table {
  border: 0;
}
.table-wrap th {
  top: 0;
}
th,
td {
  height: 2.25rem;
  padding: var(--lake-space-2) var(--lake-space-3);
  border-bottom: var(--lake-border-width) solid var(--lake-border-subtle);
  text-align: left;
  vertical-align: top;
}
th {
  position: sticky;
  z-index: 1;
  top: var(--lake-topbar-height);
  color: var(--lake-text-secondary);
  background: var(--lake-layer);
  font-size: var(--lake-text-11);
  font-weight: var(--lake-weight-semibold);
  letter-spacing: 0.06em;
  text-transform: uppercase;
}
th[aria-sort]::after {
  margin-left: var(--lake-space-1);
  content: '↕';
}
th[aria-sort='ascending']::after {
  content: '↑';
}
th[aria-sort='descending']::after {
  content: '↓';
}
tbody tr:hover {
  background: var(--lake-layer-hover);
}
tbody tr:last-child td {
  border-bottom: 0;
}
td code {
  max-width: 24rem;
  display: inline-block;
  overflow: hidden;
  text-overflow: ellipsis;
  vertical-align: bottom;
  white-space: nowrap;
}
td code:hover {
  max-width: none;
  overflow-wrap: anywhere;
  white-space: normal;
}

label {
  color: var(--lake-text-secondary);
  font-size: var(--lake-text-12);
  font-weight: var(--lake-weight-medium);
}
input:not([type='checkbox']):not([type='hidden']),
textarea,
select {
  min-height: var(--lake-control-md);
  border: var(--lake-border-width) solid var(--lake-border-strong);
  border-radius: var(--lake-control-radius);
  padding: var(--lake-space-2) var(--lake-space-3);
  color: var(--lake-text-primary);
  background: var(--lake-surface);
  font: inherit;
}
input::placeholder,
textarea::placeholder {
  color: var(--lake-text-tertiary);
}
form.login,
form.stack {
  max-width: 36rem;
  display: grid;
  gap: var(--lake-space-3);
}
form.login {
  max-width: 26rem;
}
form.login label,
form.stack label {
  display: grid;
  gap: var(--lake-space-1);
}
form.stack textarea {
  min-height: 8rem;
  resize: vertical;
}
form.search,
.filters {
  display: flex;
  align-items: end;
  flex-wrap: wrap;
  gap: var(--lake-space-2);
  margin: 0 0 var(--lake-space-6);
}
form.search input {
  min-width: min(24rem, 100%);
  flex: 1;
}
.filters label {
  display: grid;
  gap: var(--lake-space-1);
}

code,
pre,
kbd {
  font-family: var(--lake-font-mono);
  font-size: 0.9em;
}
code,
kbd {
  border-radius: var(--lake-control-radius);
  padding: 0.1rem var(--lake-space-1);
  background: var(--lake-layer);
}
pre {
  max-width: 100%;
  overflow: auto;
  padding: var(--lake-space-4);
  border: var(--lake-border-width) solid var(--lake-border-subtle);
  border-radius: var(--lake-panel-radius);
  color: var(--lake-text-primary);
  background: var(--lake-surface);
  line-height: var(--lake-leading-copy);
  white-space: pre-wrap;
  overflow-wrap: anywhere;
}
dl {
  display: grid;
  grid-template-columns: minmax(7rem, max-content) minmax(0, 1fr);
  gap: var(--lake-space-2) var(--lake-space-4);
  margin: 0;
}
dt {
  color: var(--lake-text-secondary);
}
dd {
  min-width: 0;
  margin: 0;
  overflow-wrap: anywhere;
}
.badge {
  min-height: 1.25rem;
  display: inline-flex;
  align-items: center;
  gap: var(--lake-space-1);
  border: var(--lake-border-width) solid var(--lake-border-subtle);
  border-radius: 9999px;
  padding: 0 var(--lake-space-2);
  color: var(--lake-text-secondary);
  background: var(--lake-layer);
  font-size: var(--lake-text-11);
  font-weight: var(--lake-weight-medium);
  line-height: 1;
}
.badge::before,
.status-dot {
  width: 0.375rem;
  height: 0.375rem;
  flex: 0 0 auto;
  border-radius: 50%;
  background: var(--lake-border-strong);
  content: '';
}
.status-success {
  border-color: var(--lake-success);
  color: var(--lake-success);
  background: var(--lake-success-soft);
}
.status-success::before {
  background: var(--lake-success-indicator);
}
.status-warning {
  border-color: var(--lake-warning);
  color: var(--lake-warning);
  background: var(--lake-warning-soft);
}
.status-danger {
  border-color: var(--lake-danger);
  color: var(--lake-danger);
  background: var(--lake-danger-soft);
}
.status-ai {
  border-color: var(--lake-ai);
  color: var(--lake-ai);
  background: var(--lake-ai-soft);
}
.empty {
  padding: var(--lake-space-8);
  color: var(--lake-text-secondary);
  text-align: center;
}
.muted {
  color: var(--lake-text-secondary);
}
.notice,
.toast,
.error {
  border-left: 0.1875rem solid var(--lake-info);
  padding: var(--lake-space-3) var(--lake-space-4);
  color: var(--lake-text-primary);
  background: var(--lake-info-soft);
}
.notice,
.error {
  margin: 0 0 var(--lake-space-4);
}
.error {
  border-left-color: var(--lake-danger);
  background: var(--lake-danger-soft);
}
.toast {
  position: fixed;
  z-index: var(--lake-z-toast);
  right: var(--lake-space-4);
  bottom: var(--lake-space-4);
  max-width: 28rem;
  border: var(--lake-border-width) solid var(--lake-border-subtle);
  border-left: 0.1875rem solid var(--lake-info);
  box-shadow: var(--lake-shadow-popover);
}
.diff-entry {
  margin-top: var(--lake-space-4);
  border: var(--lake-border-width) solid var(--lake-border-subtle);
  background: var(--lake-surface);
}
.diff-entry h3 {
  margin: 0;
  padding: var(--lake-space-3) var(--lake-space-4);
  border-bottom: var(--lake-border-width) solid var(--lake-border-subtle);
  font-size: var(--lake-text-14);
}
.diff-entry .badge {
  margin-left: var(--lake-space-2);
}
.diff-hunk {
  margin: 0;
  border: 0;
  border-radius: 0;
  overflow-x: auto;
}
.hunk-heading {
  padding: var(--lake-space-2) var(--lake-space-3);
  color: var(--lake-text-secondary);
  background: var(--lake-info-soft);
  font-family: var(--lake-font-mono);
}
.diff-line {
  display: grid;
  grid-template-columns: 4rem minmax(max-content, 1fr);
}
.diff-line > a {
  padding: 0 var(--lake-space-2);
  color: var(--lake-text-tertiary);
  text-align: right;
  text-decoration: none;
}
.diff-line > code {
  padding: 0 var(--lake-space-3);
  color: inherit;
  background: transparent;
  white-space: pre;
}
.diff-remove {
  color: var(--lake-danger);
  background: var(--lake-danger-soft);
}
.diff-add {
  color: var(--lake-success);
  background: var(--lake-success-soft);
}
.change-toolbar,
.review-state,
.check-row,
.file-summary li {
  display: flex;
  align-items: center;
  gap: var(--lake-space-3);
}
.change-toolbar {
  flex-wrap: wrap;
  margin-bottom: var(--lake-space-4);
}
.view-toggle {
  display: flex;
  margin-left: auto;
}
.view-toggle button {
  border-radius: 0;
}
.view-toggle button:first-child {
  border-radius: var(--lake-radius-control) 0 0 var(--lake-radius-control);
}
.view-toggle button:last-child {
  border-radius: 0 var(--lake-radius-control) var(--lake-radius-control) 0;
}
.view-toggle button[aria-pressed='true'] {
  color: var(--lake-action-text);
  background: var(--lake-action);
}
.change-message,
.review-summary {
  margin-top: var(--lake-space-5);
}
.review-state,
.check-row {
  justify-content: space-between;
}
.review-state .sub {
  margin: 0;
}
.identity-list {
  display: inline-flex;
  flex-wrap: wrap;
  gap: var(--lake-space-1);
  max-width: 100%;
}
.file-summary ul {
  margin-bottom: 0;
  padding: 0;
  list-style: none;
}
.file-summary li {
  justify-content: space-between;
  padding: var(--lake-space-2) 0;
  border-bottom: var(--lake-border-width) solid var(--lake-border-subtle);
}
.diff-stats {
  margin-left: auto;
}
.additions {
  color: var(--lake-success);
}
.deletions {
  color: var(--lake-danger);
}
.split-row {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
}
.split-row code {
  min-height: 1.4rem;
  padding: 0 var(--lake-space-3);
  white-space: pre;
  overflow-x: auto;
}
.review-comment {
  grid-column: 1 / -1;
  margin: var(--lake-space-2) var(--lake-space-4);
  padding: var(--lake-space-3);
  border: var(--lake-border-width) solid var(--lake-border-strong);
  border-radius: var(--lake-radius-card);
  color: var(--lake-text-primary);
  background: var(--lake-surface-raised);
}
.issue-list,
.discussion {
  display: grid;
  gap: var(--lake-space-3);
}
.issue-row {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: var(--lake-space-4);
}
.issue-row h2 {
  margin-bottom: var(--lake-space-1);
  font-size: var(--lake-text-16);
}
.issue-row h2 a {
  color: inherit;
  text-decoration: none;
}
.issue-row .sub {
  margin: 0;
}
.issue-layout {
  display: grid;
  grid-template-columns: minmax(0, 3fr) minmax(16rem, 1fr);
  gap: var(--lake-space-5);
}
.issue-body,
.issue-comment {
  margin-bottom: var(--lake-space-4);
}
.issue-comment .sub {
  padding-bottom: var(--lake-space-2);
  border-bottom: var(--lake-border-width) solid var(--lake-border-subtle);
}
.issue-details {
  align-self: start;
}
.issue-details .badge {
  margin: 0 var(--lake-space-1) var(--lake-space-1) 0;
}
.release-card,
.webhook-card,
.protection-rule,
.protection-event {
  margin-bottom: var(--lake-space-4);
}
.release-card > header,
.webhook-card > header,
.protection-rule > header,
.protection-event > header,
.delivery-row,
.release-artifacts li {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: var(--lake-space-4);
}
.release-card h2,
.webhook-card h2,
.protection-rule h3 {
  margin-bottom: var(--lake-space-1);
}
.release-artifacts {
  padding: 0;
  list-style: none;
}
.release-artifacts li,
.delivery-row {
  padding: var(--lake-space-3) 0;
  border-top: var(--lake-border-width) solid var(--lake-border-subtle);
}
.compact-empty {
  padding: var(--lake-space-4);
}
.delivery-row {
  align-items: center;
  flex-wrap: wrap;
}
.delivery-row .sub {
  margin: var(--lake-space-1) 0 0;
}
.state-page {
  max-width: 42rem;
  margin: var(--lake-space-8) auto;
  padding: var(--lake-space-8);
  border: var(--lake-border-width) solid var(--lake-border-subtle);
  border-radius: var(--lake-radius-card);
  background: var(--lake-surface);
  text-align: center;
}
.error-state {
  border-top: 0.1875rem solid var(--lake-danger);
}
.loading-bars {
  display: grid;
  gap: var(--lake-space-2);
  margin-top: var(--lake-space-5);
}
.loading-bars span {
  height: var(--lake-space-3);
  border-radius: var(--lake-radius-control);
  background: var(--lake-border-subtle);
  animation: lake-loading var(--lake-motion-deliberate) var(--lake-motion-ease) infinite alternate;
}
.loading-bars span:nth-child(2) {
  width: 78%;
}
.loading-bars span:nth-child(3) {
  width: 56%;
}
@keyframes lake-loading {
  to {
    opacity: 0.35;
  }
}
.image-preview {
  max-width: 100%;
  max-height: 40rem;
  display: block;
  margin: auto;
}
.file-preview {
  max-height: 40rem;
  overflow: auto;
}
.forge-heading,
.section-heading,
.change-list li,
.detail-list li,
.language-summary li {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--lake-space-3);
}
.forge-heading {
  align-items: start;
}
.forge-tabs {
  margin: var(--lake-space-6) 0 var(--lake-space-4);
  overflow-x: auto;
  flex-wrap: nowrap;
}
.forge-tabs a {
  flex: 0 0 auto;
}
.repository-layout {
  display: grid;
  grid-template-columns: minmax(0, 1fr) minmax(17rem, 21rem);
  gap: var(--lake-space-4);
  align-items: start;
}
.repository-main,
.repository-sidebar {
  min-width: 0;
}
.repository-sidebar {
  display: grid;
  gap: var(--lake-space-4);
}
.repository-sidebar .panel h2,
.repository-browser h2,
.recent-changes h2,
.readme-panel h2 {
  margin: 0;
  font-size: var(--lake-text-16);
}
.section-heading {
  align-items: baseline;
  margin-bottom: var(--lake-space-3);
}
.section-heading p {
  margin: var(--lake-space-1) 0 0;
}
.tree-group {
  border-top: var(--lake-border-width) solid var(--lake-border-subtle);
}
.tree-group summary {
  min-height: 2.5rem;
  display: flex;
  align-items: center;
  padding: 0 var(--lake-space-2);
  color: var(--lake-text-secondary);
  cursor: pointer;
}
.forge-tree,
.change-list,
.detail-list,
.language-summary {
  margin: 0;
  padding: 0;
  list-style: none;
}
.forge-tree li {
  min-height: 2.5rem;
  display: grid;
  grid-template-columns: 1rem minmax(0, 1fr) auto;
  align-items: center;
  gap: var(--lake-space-2);
  border-top: var(--lake-border-width) solid var(--lake-border-subtle);
}
.tree-kind,
.tree-size {
  color: var(--lake-text-tertiary);
}
.tree-size {
  font-family: var(--lake-font-mono);
  font-size: var(--lake-text-12);
}
.change-list li,
.detail-list li,
.language-summary li {
  min-height: 2.75rem;
  border-top: var(--lake-border-width) solid var(--lake-border-subtle);
}
.change-list li > div,
.detail-list li > div {
  min-width: 0;
  display: grid;
}
.change-list .muted {
  font-size: var(--lake-text-12);
}
.identifier {
  max-width: 18rem;
  display: inline-block;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.compact-empty {
  padding: var(--lake-space-5);
}
.markdown-body {
  max-width: 78ch;
}
.markdown-body h1,
.markdown-body h2,
.markdown-body h3,
.markdown-body h4,
.markdown-body h5,
.markdown-body h6 {
  scroll-margin-top: calc(var(--lake-topbar-height) + var(--lake-space-3));
}
.markdown-body h1 {
  margin-top: var(--lake-space-6);
  font-size: var(--lake-text-24);
}
.heading-anchor {
  margin-left: var(--lake-space-2);
  color: var(--lake-text-tertiary);
  text-decoration: none;
  opacity: 0;
}
.markdown-body :is(h1, h2, h3, h4, h5, h6):hover .heading-anchor,
.heading-anchor:focus-visible {
  opacity: 1;
}
.repository-sidebar pre {
  padding: var(--lake-space-3);
  white-space: pre-wrap;
  overflow-wrap: anywhere;
}
.repository-sidebar pre code {
  padding: 0;
  background: transparent;
}
.tree-browser {
  padding: 0;
  overflow: hidden;
}
.tree-browser-heading,
.tree-view li {
  display: grid;
  grid-template-columns: minmax(0, 1fr) minmax(12rem, 30%);
  gap: var(--lake-space-4);
  align-items: center;
}
.tree-browser-heading {
  min-height: 2.5rem;
  padding: 0 var(--lake-space-4);
  color: var(--lake-text-secondary);
  background: var(--lake-layer);
  font-size: var(--lake-text-11);
  font-weight: var(--lake-weight-semibold);
  letter-spacing: 0.06em;
  text-transform: uppercase;
}
.tree-view {
  margin: 0;
  padding: 0;
  list-style: none;
}
.tree-view li {
  min-height: 3.25rem;
  padding: var(--lake-space-2) var(--lake-space-4);
  border-top: var(--lake-border-width) solid var(--lake-border-subtle);
}
.tree-view li:hover {
  background: var(--lake-layer-hover);
}
.tree-entry-name {
  min-width: 0;
  display: grid;
  grid-template-columns: 1rem minmax(0, 1fr);
  gap: var(--lake-space-2);
  align-items: start;
}
.tree-entry-meta,
.tree-last-change {
  display: block;
  color: var(--lake-text-secondary);
  font-size: var(--lake-text-12);
}
.tree-last-change {
  min-width: 0;
  display: grid;
  gap: var(--lake-space-1);
}
.file-heading {
  display: flex;
  align-items: start;
  justify-content: space-between;
  gap: var(--lake-space-4);
}
.file-meta {
  min-height: 2.75rem;
  display: flex;
  align-items: center;
  flex-wrap: wrap;
  gap: var(--lake-space-3);
  padding: 0 var(--lake-space-3);
  border: var(--lake-border-width) solid var(--lake-border-subtle);
  border-bottom: 0;
  color: var(--lake-text-secondary);
  background: var(--lake-layer);
  font-size: var(--lake-text-12);
}
.source-view {
  max-width: 100%;
  overflow: auto;
  border: var(--lake-border-width) solid var(--lake-border-subtle);
  background: var(--lake-surface);
}
.source-view table {
  min-width: 52rem;
  border: 0;
  table-layout: fixed;
}
.source-view tr:hover,
.source-view tr.selected,
.source-view tr:target {
  background: var(--lake-selected);
}
.source-view td {
  height: auto;
  padding-top: 0;
  padding-bottom: 0;
  border: 0;
  line-height: 1.65;
}
.blame-cell {
  width: 15rem;
  padding-left: var(--lake-space-3);
  color: var(--lake-text-tertiary);
  background: var(--lake-layer);
  font-size: var(--lake-text-11);
}
.blame-cell .identifier,
.blame-cell span,
.blame-cell time {
  display: block;
}
.line-number {
  width: 4rem;
  padding-right: var(--lake-space-2);
  color: var(--lake-text-tertiary);
  background: var(--lake-layer);
  text-align: right;
  user-select: none;
}
.line-number a {
  color: inherit;
  text-decoration: none;
}
.source-line {
  padding-right: var(--lake-space-4);
  padding-left: var(--lake-space-3);
  white-space: pre;
}
.source-line code {
  padding: 0;
  color: inherit;
  background: transparent;
}
.syntax-keyword {
  color: var(--lake-ai);
  font-weight: var(--lake-weight-medium);
}
.syntax-string {
  color: var(--lake-success);
}
.syntax-comment {
  color: var(--lake-text-tertiary);
  font-style: italic;
}
.syntax-unit .source-line {
  box-shadow: inset 0.125rem 0 var(--lake-border-subtle);
}

@media (max-width: 58rem) {
  .app-shell {
    display: block;
    background: none;
  }
  .sidebar {
    position: static;
    width: auto;
    height: auto;
    border-right: 0;
    border-bottom: var(--lake-border-width) solid var(--lake-border-subtle);
  }
  .primary-nav {
    display: flex;
    gap: var(--lake-space-1);
    overflow-x: auto;
  }
  .nav-label,
  .sidebar-footer {
    display: none;
  }
  .nav-link {
    flex: 0 0 auto;
    border-left: 0;
    border-bottom: 0.125rem solid transparent;
  }
  .nav-link[aria-current='page'] {
    border-bottom-color: var(--lake-action);
  }
  .topbar {
    top: 0;
  }
  th {
    top: var(--lake-topbar-height);
  }
  .repository-layout {
    grid-template-columns: 1fr;
  }
  .issue-layout {
    grid-template-columns: 1fr;
  }
  .tree-browser-heading,
  .tree-view li {
    grid-template-columns: minmax(0, 1fr);
  }
  .tree-browser-heading span:last-child {
    display: none;
  }
}

@media (max-width: 40rem) {
  .topbar,
  .hosted-header,
  .hosted-main,
  .page-container {
    padding-right: var(--lake-page-gutter-compact);
    padding-left: var(--lake-page-gutter-compact);
  }
  .topbar-trail li:not(:nth-last-child(-n + 2)) {
    display: none;
  }
  h1 {
    font-size: var(--lake-text-24);
  }
  dl {
    grid-template-columns: 1fr;
    gap: var(--lake-space-1);
  }
  dd {
    margin-bottom: var(--lake-space-2);
  }
  .file-heading {
    display: block;
  }
  form.search input,
  .filters label,
  .filters input,
  .filters select {
    width: 100%;
  }
}

@media (prefers-reduced-motion: reduce) {
  html {
    scroll-behavior: auto;
  }
  *,
  *::before,
  *::after {
    animation-duration: 0.01ms !important;
    animation-iteration-count: 1 !important;
    transition-duration: 0.01ms !important;
  }
}
/* Pipeline graph (docs/design/zetta-parity.md): server-rendered SVG, one lane per stage. */
.pipeline-canvas {
  overflow-x: auto;
  border: var(--lake-border-width) solid var(--lake-border-subtle);
  border-radius: var(--lake-radius-control);
  padding: var(--lake-space-3);
  background: var(--lake-surface);
}
.pipeline-graph {
  display: block;
  width: 100%;
  min-width: 48rem;
  height: auto;
  color: var(--lake-text-tertiary);
  font-family: var(--lake-font-sans);
}
.pipeline-graph .lane {
  fill: var(--lake-text-tertiary);
  font-size: var(--lake-text-11);
  font-weight: var(--lake-weight-semibold);
  text-transform: uppercase;
  letter-spacing: 0.04em;
}
.pipeline-graph .edge {
  stroke: var(--lake-border-strong);
  stroke-width: 1.5;
}
.pipeline-graph .edge-control {
  stroke-dasharray: 5 4;
}
.pipeline-graph .step rect {
  fill: var(--lake-layer);
  stroke: var(--lake-border-strong);
  stroke-width: 1;
}
.pipeline-graph a:hover .step rect,
.pipeline-graph .step.selected rect {
  stroke: var(--lake-action);
  stroke-width: 2;
}
.pipeline-graph .step.status-succeeded rect {
  fill: var(--lake-success-soft);
  stroke: var(--lake-success);
}
.pipeline-graph .step.status-failed rect {
  fill: var(--lake-danger-soft);
  stroke: var(--lake-danger);
}
.pipeline-graph .step.status-running rect {
  fill: var(--lake-warning-soft);
  stroke: var(--lake-warning);
}
.pipeline-graph .step-name {
  fill: var(--lake-text-primary);
  font-size: var(--lake-text-12);
  font-weight: var(--lake-weight-semibold);
}
.pipeline-graph .step-meta {
  fill: var(--lake-text-secondary);
  font-family: var(--lake-font-mono);
  font-size: var(--lake-text-11);
}
.pipeline-edit {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(16rem, 1fr));
  gap: var(--lake-space-4);
  align-items: start;
}
.pipeline-edit > form {
  min-width: 0;
}
.pipeline-edit select,
.pipeline-edit input,
.pipeline-edit textarea {
  width: 100%;
  min-width: 0;
}
.gate-checks td:first-child {
  font-family: var(--lake-font-mono);
}
.waterfall-bar {
  display: inline-block;
  height: 0.5rem;
  border-radius: 2px;
  background: var(--lake-action);
}
.waterfall-bar.failed {
  background: var(--lake-danger);
}
)lake_styles"
