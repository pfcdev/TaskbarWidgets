const { invoke } = window.__TAURI__.core;

const widgetPresentation = {
  "codex-status": { icon: "terminal", accent: "#5fd4ff", featured: true },
  "weather-static": { icon: "partly_cloudy_day", accent: "#f59e0b", featured: true },
  "discord-voice": { icon: "forum", accent: "#5865f2" },
  "media-player": { icon: "play_circle", accent: "#1db954" },
  "steam-download": { icon: "download", accent: "#66c0f4", featured: true },
};

const widgetCatalog = (window.TASKBAR_WIDGET_CATALOG || []).map((manifest) => ({
  ...manifest,
  title: manifest.displayName,
  ...(widgetPresentation[manifest.id] || { icon: "widgets", accent: "#5fd4ff" }),
}));

const defaultWidgets = widgetCatalog.map((widget, index) => ({
  id: widget.id,
  design: widget.id,
  enabled: widget.id === "codex-status",
  moveX: 0,
  positionPct: 100,
  order: index,
}));

const defaults = {
  activeDesign: "codex-status",
  enabled: true,
  refreshIntervalSecs: 30,
  widgetOffsetPx: 0,
  widgetMoveX: 0,
  widgets: defaultWidgets,
  rotationEnabled: false,
  rotationIntervalSecs: 30,
  rotationDesigns: widgetCatalog.map((item) => item.id),
  codexApiEndpoint: "",
  codexProjectFilter: "",
  weatherCity: "Istanbul",
  weatherTempUnit: "C",
  discordEnabled: false,
  discordBackgroundEnabled: true,
  mediaDarkMode: true,
  discordClientId: "",
  discordClientSecret: "",
  discordRedirectUri: "http://127.0.0.1/callback",
};

const pageMeta = {
  library: {
    title: "Widget Library",
    description: "Browse TaskbarWidgets widgets, enable taskbar surfaces, and tune per-widget behavior.",
  },
  rotation: {
    title: "Slider Rotation",
    description: "Configure the active queue, timing, and taskbar preview for rotating widgets.",
  },
  updates: {
    title: "System Updates",
    description: "Manage the Stable release channel, check GitHub releases, and install updates.",
  },
  settings: {
    title: "Settings",
    description: "Manage TaskbarWidgets behavior, widget settings, and system integrations.",
  },
};

let state = {
  appDir: "",
  page: "library",
  settings: { ...defaults },
  updateStatus: {},
  mediaStatus: {},
  releaseTimeline: [],
  releaseTimelineState: "idle",
  search: "",
  dirty: false,
  status: "",
  previewMode: "bottom",
  previewIndex: 0,
  modalWidgetId: "",
};

let autosaveTimer = 0;
let previewTimer = 0;
let updatePollTimer = 0;
let updateInstallerLaunchInProgress = false;
let updateInstallRequested = false;

function widgetById(id) {
  return widgetCatalog.find((item) => item.id === id) || widgetCatalog[0];
}

function isKnownWidget(id) {
  return widgetCatalog.some((item) => item.id === id);
}

function mergeSettings(settings) {
  const merged = { ...defaults, ...settings };
  merged.activeDesign = widgetById(merged.activeDesign).id;
  merged.rotationDesigns = normalizeRotation(merged.rotationDesigns);
  merged.widgetOffsetPx = clampNumber(merged.widgetOffsetPx, 0, 480, 0);
  merged.widgetMoveX = Object.prototype.hasOwnProperty.call(settings || {}, "widgetMoveX")
    ? clampNumber(merged.widgetMoveX, -640, 640, 0)
    : -merged.widgetOffsetPx;
  merged.widgets = normalizeWidgets(
    merged.widgets,
    merged.activeDesign,
    merged.enabled,
    merged.widgetMoveX,
  );
  merged.refreshIntervalSecs = clampNumber(merged.refreshIntervalSecs, 1, 3600, 30);
  merged.rotationIntervalSecs = clampNumber(merged.rotationIntervalSecs, 5, 3600, 30);
  return merged;
}

function normalizeWidgets(list, activeDesign, legacyEnabled = true, legacyMoveX = 0) {
  const source = Array.isArray(list) && list.length
    ? list
    : defaultWidgets.map((widget) => ({
        ...widget,
        enabled: widget.design === activeDesign ? Boolean(legacyEnabled) : false,
        moveX: widget.design === activeDesign ? clampNumber(legacyMoveX, -640, 640, 0) : 0,
        positionPct: 100,
      }));

  const result = [];
  for (const item of source) {
    const requested = item.design || item.designId || item.id;
    const design = isKnownWidget(requested) ? requested : String(requested || "unknown-widget");
    if (result.some((widget) => widget.design === design)) continue;
    result.push({
      id: item.id || design,
      design,
      enabled: isKnownWidget(design) && Boolean(item.enabled),
      moveX: clampNumber(item.moveX ?? item.widgetMoveX ?? 0, -640, 640, 0),
      positionPct: clampNumber(item.positionPct ?? 100, 0, 100, 100),
      order: clampNumber(item.order ?? result.length, 0, 1000, result.length),
    });
  }

  for (const widget of defaultWidgets) {
    if (!result.some((item) => item.design === widget.design)) {
      result.push({ ...widget, enabled: false, moveX: 0, positionPct: 100, order: result.length });
    }
  }

  return result
    .sort((left, right) => left.order - right.order)
    .map((widget, index) => ({ ...widget, order: index }));
}

function normalizeRotation(list) {
  const source = Array.isArray(list) && list.length ? list : defaults.rotationDesigns;
  const result = [];
  for (const id of source) {
    const normalized = widgetById(id).id;
    if (!result.includes(normalized)) result.push(normalized);
  }
  return result.length ? result : ["codex-status"];
}

function activeWidget() {
  return widgetState(state.settings.activeDesign);
}

function widgetState(id) {
  const design = widgetById(id).id;
  let widget = state.settings.widgets.find((item) => item.design === design);
  if (!widget) {
    widget = {
      id: design,
      design,
      enabled: false,
      moveX: 0,
      positionPct: 100,
      order: state.settings.widgets.length,
    };
    state.settings.widgets.push(widget);
  }
  return widget;
}

function enabledWidgets() {
  return state.settings.widgets
    .filter((widget) => widget.enabled)
    .sort((left, right) => left.order - right.order)
    .map((widget) => widget.design);
}

function currentPreviewDesign() {
  const rotationQueue = state.settings.rotationEnabled
    ? state.settings.rotationDesigns.filter((id) => state.settings.widgets.some((widget) => widget.design === id && widget.enabled))
    : [];
  const queue = rotationQueue.length ? rotationQueue : enabledWidgets();
  if (!queue.length) return state.settings.activeDesign;
  return queue[state.previewIndex % queue.length];
}

function clampNumber(value, min, max, fallback) {
  const number = Number.parseInt(value, 10);
  if (!Number.isFinite(number)) return fallback;
  return Math.min(max, Math.max(min, number));
}

async function boot() {
  try {
    const loaded = await invoke("load_state");
    state.appDir = loaded.appDir || "";
    state.settings = mergeSettings(loaded.settings || {});
    state.updateStatus = loaded.updateStatus || {};
    state.mediaStatus = loaded.mediaStatus || {};
  } catch (error) {
    state.status = `Load failed: ${error}`;
  }
  render();
  loadReleaseTimeline();
  startPreviewLoop();
  if (isUpdateBusy(state.updateStatus)) startUpdatePolling();
}

function render() {
  renderNavigation();
  renderPage();
  renderFloatingTaskbar();
  renderWidgetModal();
  localizeIcons();
}

function localizeIcons() {
  const glyphs = {
    widgets: "▦", terminal: "⌘", partly_cloudy_day: "☀", forum: "●",
    play_circle: "▶", download: "↓", rebase_edit: "⇄", system_update: "↻",
    settings: "⚙", help: "?", chat_bubble: "◌", search: "⌕",
    pending: "◷", check_circle: "✓", check: "✓", add: "+", view_timeline: "≡",
    desktop_windows: "▣", drag_indicator: "⋮", keyboard_arrow_up: "↑",
    keyboard_arrow_down: "↓", close: "×", tune: "⚙", extension: "◇",
    warning: "!", new_releases: "★", hourglass_top: "◷", sync: "↻",
    system_update_alt: "↓", history: "↶", save: "✓", folder_open: "□",
    grid_view: "▦", folder: "□", language: "◎", expand_less: "⌃", wifi: "⌁",
    volume_up: "◖", window: "⊞", eject: "■", play_arrow: "▶", light_mode: "☀"
  };
  document.querySelectorAll(".material-symbols-outlined:not([data-localized-icon])").forEach((element) => {
    const name = element.textContent.trim();
    element.textContent = glyphs[name] || "◇";
    element.dataset.localizedIcon = name || "unknown";
    element.setAttribute("aria-hidden", "true");
  });
}

function renderNavigation() {
  document.querySelectorAll(".nav-item[data-page]").forEach((button) => {
    button.classList.toggle("active", button.dataset.page === state.page);
    button.onclick = () => {
      state.page = button.dataset.page;
      state.status = "";
      render();
      if (state.page === "updates" && !state.releaseTimeline.length) {
        loadReleaseTimeline();
      }
    };
  });
}

function renderPage() {
  const page = document.getElementById("page");
  if (state.page === "rotation") {
    page.innerHTML = rotationPage();
    bindRotationPage();
  } else if (state.page === "updates") {
    page.innerHTML = updatesPage();
    bindUpdatesPage();
  } else if (state.page === "settings") {
    page.innerHTML = settingsPage();
    bindSettingsPage();
  } else {
    page.innerHTML = libraryPage();
    bindLibraryPage();
  }
  localizeIcons();
}

function pageHeader(pageId = state.page) {
  const meta = pageMeta[pageId];
  return `
    <header class="page-header">
      <div>
        <h2>${escapeHtml(meta.title)}</h2>
        <p>${escapeHtml(meta.description)}</p>
      </div>
      <div class="save-pill ${state.dirty ? "dirty" : ""}">
        <span class="material-symbols-outlined">${state.dirty ? "pending" : "check_circle"}</span>
        ${state.dirty ? "Unsaved" : "Saved"}
      </div>
    </header>
  `;
}

function libraryPage() {
  const filtered = widgetCatalog.filter((widget) => {
    const q = state.search.trim().toLowerCase();
    if (!q) return true;
    return [widget.title, widget.category, widget.description].join(" ").toLowerCase().includes(q);
  });
  return `
    ${pageHeader("library")}
    <section class="library-toolbar">
      <div class="search-box">
        <span class="material-symbols-outlined">search</span>
        <input id="widget-search" value="${escapeAttr(state.search)}" placeholder="Search widgets..." />
      </div>
      <span class="library-count">${filtered.length} widget${filtered.length === 1 ? "" : "s"}</span>
    </section>

    <section class="widget-library-list" aria-label="Available widgets">
      ${filtered.length
        ? filtered.map(widgetLibraryRow).join("")
        : `<div class="library-empty"><strong>No widgets found</strong><span>Try a different search.</span></div>`}
    </section>
  `;
}

function widgetLibraryRow(widget) {
  const runtime = widgetRuntime(widget.id);
  const enabled = isWidgetEnabled(widget.id);
  return `
    <article class="widget-library-row ${enabled ? "enabled" : ""}" style="--accent:${widget.accent}">
      <button class="native-preview-button" data-open-widget="${widget.id}" type="button" aria-label="Open ${escapeAttr(widget.title)} settings">
        ${runtime.preview}
      </button>
      <div class="widget-library-copy">
        <div class="widget-library-title">
          <span class="widget-accent-dot"></span>
          <h3>${escapeHtml(widget.title)}</h3>
          <span>${escapeHtml(widget.category)}</span>
        </div>
        <p>${escapeHtml(widget.description)}</p>
      </div>
      ${toggleButton(widget.id, true)}
    </article>
  `;
}

function toggleButton(id, compact = false) {
  const enabled = isWidgetEnabled(id);
  return `
    <button class="${compact ? "round-action widget-check-action" : "gradient-action"} ${enabled ? "active" : ""}" data-toggle-widget="${id}" type="button" aria-label="${enabled ? "Disable" : "Enable"} ${escapeAttr(widgetById(id).title)}">
      <span class="material-symbols-outlined">${enabled ? "check" : "add"}</span>
      ${compact ? "" : `<span>${enabled ? "Added" : "Add"}</span>`}
    </button>
  `;
}

function rotationPage() {
  const queue = state.settings.rotationDesigns;
  return `
    ${pageHeader("rotation")}
    <section class="rotation-header">
      <div>
        <h3>Active Sequence</h3>
        <p>Choose widgets and arrange the order shown on the taskbar preview.</p>
      </div>
      ${settingToggle("rotationEnabled", "Enable Rotation", state.settings.rotationEnabled, "large")}
    </section>

    <div class="rotation-layout">
      <section class="sequence-column">
        <div class="sequence-head">
          <h3><span class="material-symbols-outlined">view_timeline</span> Active Sequence</h3>
          <button class="text-action" id="enable-all-rotation" type="button">
            <span class="material-symbols-outlined">add</span>
            Add Widgets
          </button>
        </div>
        <div class="sequence-list">
          ${queue.map((id, index) => sequenceItem(id, index)).join("")}
          <div class="drop-zone">Drop new widget here</div>
        </div>
      </section>

      <section class="preview-column">
        <div class="sequence-head">
          <h3><span class="material-symbols-outlined">desktop_windows</span> Live Taskbar Preview</h3>
          <div class="segmented">
            <button class="${state.previewMode === "bottom" ? "active" : ""}" data-preview-mode="bottom" type="button">Bottom</button>
            <button class="${state.previewMode === "rail" ? "active" : ""}" data-preview-mode="rail" type="button">Left Rail</button>
          </div>
        </div>
        <div class="desktop-preview ${state.previewMode === "rail" ? "rail" : ""}">
          <div class="desktop-glow"></div>
          ${desktopTaskbarMarkup()}
        </div>
        <div class="interval-card glass-panel">
          <label>Slide Interval</label>
          <div>
            <input type="number" min="5" max="3600" data-setting="rotationIntervalSecs" value="${escapeAttr(state.settings.rotationIntervalSecs)}" />
            <span>sec</span>
          </div>
        </div>
      </section>
    </div>
    ${inlineStatus()}
  `;
}

function sequenceItem(id, index) {
  const widget = widgetById(id);
  const active = index === state.previewIndex % Math.max(1, state.settings.rotationDesigns.length);
  return `
    <article class="sequence-item ${active ? "current" : ""}" draggable="true" data-sequence-id="${widget.id}" style="--accent:${widget.accent}">
      <div class="drag-handle"><span class="material-symbols-outlined">drag_indicator</span></div>
      <div class="sequence-icon"><span class="material-symbols-outlined">${widget.icon}</span></div>
      <div class="sequence-copy">
        <strong>${escapeHtml(widget.title)}</strong>
        <small>${active ? "Currently Active" : `Queue #${index + 1}`}</small>
      </div>
      <div class="sequence-controls">
        <input type="number" min="5" max="3600" data-setting="rotationIntervalSecs" value="${escapeAttr(state.settings.rotationIntervalSecs)}" />
        <span>sec</span>
      </div>
      <select data-transition="${widget.id}">
        <option value="fade">Fade</option>
        <option value="slide_up" selected>Slide Up</option>
        <option value="slide_left">Slide Left</option>
      </select>
      <button class="icon-button" data-move="${widget.id}" data-dir="-1" ${index === 0 ? "disabled" : ""} type="button">
        <span class="material-symbols-outlined">keyboard_arrow_up</span>
      </button>
      <button class="icon-button" data-move="${widget.id}" data-dir="1" ${index === state.settings.rotationDesigns.length - 1 ? "disabled" : ""} type="button">
        <span class="material-symbols-outlined">keyboard_arrow_down</span>
      </button>
      <button class="icon-button danger" data-remove-rotation="${widget.id}" type="button">
        <span class="material-symbols-outlined">close</span>
      </button>
    </article>
  `;
}

function settingsPage() {
  return `
    ${pageHeader("settings")}
    <div class="settings-stack">
      <section class="glass-panel settings-section">
        <div class="section-title"><span class="material-symbols-outlined">tune</span><h3>Current Widget Settings</h3></div>
        <div class="segmented wide">
          ${widgetCatalog.map((widget) => `
            <button class="${state.settings.activeDesign === widget.id ? "active" : ""}" data-select-widget="${widget.id}" type="button">${escapeHtml(widget.title)}</button>
          `).join("")}
        </div>
        ${currentWidgetSettingsFields()}
      </section>

      <section class="glass-panel settings-section">
        <div class="section-title"><span class="material-symbols-outlined">extension</span><h3>Explorer Integration</h3></div>
        ${runtimeControlPanel()}
      </section>

      <section class="glass-panel settings-section danger-section">
        <div class="section-title"><span class="material-symbols-outlined">warning</span><h3>Danger Zone</h3></div>
        <div class="setting-row">
          <div><strong>Reset to Saved Settings</strong><p>Discard unsaved local edits and reload the settings file.</p></div>
          <button class="outline-danger" id="reset-settings" type="button">Reset</button>
        </div>
      </section>
    </div>
    ${actionFooter()}
  `;
}

function currentWidgetSettingsFields() {
  const id = state.settings.activeDesign;
  if (id === "weather-static") {
    return `
      ${textSetting("weatherCity", "City", "Weather location name.", state.settings.weatherCity, "Istanbul")}
      ${selectSetting("weatherTempUnit", "Temperature Unit", "Display format.", state.settings.weatherTempUnit, [["C", "Celsius"], ["F", "Fahrenheit"]])}
    `;
  }
  if (id === "discord-voice") {
    return `
      ${settingToggle("discordEnabled", "Discord Integration", state.settings.discordEnabled)}
      ${settingToggle("discordBackgroundEnabled", "Widget Background", state.settings.discordBackgroundEnabled)}
      ${textSetting("discordClientId", "Client ID", "Discord application client id.", state.settings.discordClientId, "1525972653641433288")}
      ${textSetting("discordClientSecret", "Client Secret", "Stored in Data/config.json.", state.settings.discordClientSecret, "client secret", true)}
      ${textSetting("discordRedirectUri", "Redirect URI", "Must match Discord Developer Portal.", state.settings.discordRedirectUri, "http://127.0.0.1/callback")}
    `;
  }
  if (id === "media-player") {
    return `
      ${settingToggle("mediaDarkMode", "Dark Mode", state.settings.mediaDarkMode)}
      ${mediaDiagnostics()}
    `;
  }
  if (id === "steam-download") {
    return `
      <div class="setting-row">
        <div><strong>Steam Source</strong><p>Reads local Steam manifests and content logs from the installed Steam client.</p></div>
        <span class="status-chip">Automatic</span>
      </div>
    `;
  }
  if (id === "weather-static") return "";
  return `
    ${textSetting("codexApiEndpoint", "API Endpoint", "Custom API endpoint URL.", state.settings.codexApiEndpoint, "https://api.example.com")}
    ${textSetting("codexProjectFilter", "Project Filter", "Filter displayed projects by name.", state.settings.codexProjectFilter, "my-project")}
  `;
}

function updatesPage() {
  const update = state.updateStatus || {};
  const busy = isUpdateBusy(update);
  const downloading = update.state === "downloading";
  const installing = update.state === "installing" || updateInstallerLaunchInProgress;
  const current = update.currentVersion || "0.3.1";
  const latest = update.latestVersion || "Not checked";
  const checked = update.updatedAtUnix ? formatUnixTime(update.updatedAtUnix) : "Not checked";
  const isCurrent = update.state === "current" || (latest !== "Not checked" && latest.replace(/^v/i, "") === current.replace(/\.0$/, ""));
  return `
    ${pageHeader("updates")}
    <div class="updates-status glass-panel">
      <div class="status-orb ${isCurrent ? "ok" : "pending"}"><span class="material-symbols-outlined filled">${isCurrent ? "check_circle" : "new_releases"}</span></div>
      <div>
        <h3>${isCurrent ? "System is up to date" : update.updateAvailable ? "Update available" : "Release status"}</h3>
        <p>Checked: ${escapeHtml(checked)}</p>
      </div>
    </div>

    <div class="updates-layout">
      <section class="glass-panel channel-card">
        <h3>Update Channel</h3>
        <div class="segmented">
          <button class="active" type="button">Stable</button>
          <button disabled type="button">Preview</button>
          <button disabled type="button">Dev</button>
        </div>
        <p>Currently on <strong>Stable</strong> channel. Preview and Dev channels are not available in this build.</p>
      </section>

      <section class="glass-panel update-card inner-glow">
        <div>
          <span class="status-chip">${update.updateAvailable ? "Available" : "Stable"}</span>
          <h3>${escapeHtml(latest)}</h3>
          <p>Current: ${escapeHtml(current)}</p>
        </div>
        <div class="update-actions">
          <button class="gradient-action" id="check-updates" ${busy ? "disabled" : ""} type="button">
            <span class="material-symbols-outlined">${busy ? "hourglass_top" : "sync"}</span>
            <span>${downloading ? "Downloading" : busy ? "Checking" : "Check Updates"}</span>
          </button>
          <button class="secondary-action" id="install-update" ${!update.updateAvailable || busy || installing ? "disabled" : ""} type="button">
            <span class="material-symbols-outlined">${installing ? "hourglass_top" : "system_update_alt"}</span>
            <span>${installing ? "Starting Setup" : downloading ? "Downloading" : "Install Update"}</span>
          </button>
        </div>
        <p class="${busy ? "pulse" : ""}">${escapeHtml(update.message || "Run a check to refresh update status.")}</p>
        ${updateProgressMarkup(update)}
      </section>

      <section class="glass-panel release-timeline">
        <h3><span class="material-symbols-outlined">history</span> Release Timeline</h3>
        <div class="timeline-list">
          ${releaseTimelineMarkup()}
        </div>
      </section>
    </div>
    ${inlineStatus()}
  `;
}

function updateProgressMarkup(update) {
  if (update.state !== "downloading" && update.progressPercent == null) return "";
  const percent = Number.isFinite(Number(update.progressPercent))
    ? Math.min(100, Math.max(0, Number(update.progressPercent)))
    : 0;
  const sizeText = update.downloadedBytes || update.totalBytes
    ? `${formatBytes(update.downloadedBytes || 0)}${update.totalBytes ? ` / ${formatBytes(update.totalBytes)}` : ""}`
    : "Preparing download...";
  return `
    <div class="download-progress" aria-label="Update download progress">
      <div class="download-progress-head">
        <span>${escapeHtml(sizeText)}</span>
        <strong>${Number.isFinite(percent) ? `${percent.toFixed(percent % 1 ? 1 : 0)}%` : ""}</strong>
      </div>
      <div class="download-progress-track"><i style="width:${escapeAttr(percent)}%"></i></div>
    </div>
  `;
}

function releaseTimelineMarkup() {
  if (state.releaseTimelineState === "loading") {
    return `<div class="timeline-empty pulse">Loading local update status...</div>`;
  }
  const items = state.releaseTimeline.length ? state.releaseTimeline : fallbackTimeline();
  return items.map((release, index) => `
    <article class="timeline-item ${index === 0 ? "current" : ""}">
      <div class="timeline-dot"></div>
      <div class="timeline-head">
        <h4>${escapeHtml(release.name || release.tagName)} ${index === 0 ? "<span>(Latest)</span>" : ""}</h4>
        <time>${escapeHtml(formatDate(release.publishedAt || release.createdAt))}</time>
      </div>
      <div class="timeline-body">${releaseBodyMarkup(release.body)}</div>
    </article>
  `).join("");
}

function releaseBodyMarkup(body) {
  const text = String(body || "Taskbar Widgets release build. Use TaskbarWidgetsSetup-x64.exe for normal Windows installation/update.").trim();
  const lines = text.split(/\r?\n/).filter(Boolean).slice(0, 4);
  return `<p>${escapeHtml(lines.join(" "))}</p>`;
}

function fallbackTimeline() {
  const update = state.updateStatus || {};
  return [
    {
      tagName: update.latestVersion || "v0.1.5",
      name: update.latestVersion ? `Taskbar Widgets ${update.latestVersion}` : "Taskbar Widgets",
      publishedAt: update.updatedAtUnix ? new Date(update.updatedAtUnix * 1000).toISOString() : new Date().toISOString(),
      body: update.message || "Use Check for updates to refresh release status.",
    },
  ];
}

function settingToggle(key, label, checked, mode = "setting") {
  return `
    <div class="setting-row">
      <div><strong>${escapeHtml(label)}</strong><p>${toggleHint(key)}</p></div>
      <label class="win-toggle">
        <input type="checkbox" data-${mode === "widget" ? "widget-" : ""}setting="${key}" ${checked ? "checked" : ""} />
        <span><i></i></span>
      </label>
    </div>
  `;
}

function rangeSetting(key, label, hint, value, min, max, unit = "", mode = "setting") {
  return `
    <div class="setting-block">
      <div class="setting-head"><div><strong>${escapeHtml(label)}</strong><p>${escapeHtml(hint)}</p></div><span id="${mode}-${key}-value">${value}${unit}</span></div>
      <input type="range" min="${min}" max="${max}" step="1" data-${mode === "widget" ? "widget-" : ""}setting="${key}" data-unit="${escapeAttr(unit)}" value="${escapeAttr(value)}" />
    </div>
  `;
}

function numberSetting(key, label, hint, value, min, max) {
  return `
    <div class="setting-row">
      <div><strong>${escapeHtml(label)}</strong><p>${escapeHtml(hint)}</p></div>
      <input class="compact-input" type="number" min="${min}" max="${max}" data-setting="${key}" value="${escapeAttr(value)}" />
    </div>
  `;
}

function textSetting(key, label, hint, value, placeholder, secret = false) {
  return `
    <div class="setting-block">
      <div class="setting-head"><div><strong>${escapeHtml(label)}</strong><p>${escapeHtml(hint)}</p></div></div>
      <input class="text-input" type="${secret ? "password" : "text"}" data-setting="${key}" value="${escapeAttr(value || "")}" placeholder="${escapeAttr(placeholder)}" />
    </div>
  `;
}

function selectSetting(key, label, hint, value, options) {
  return `
    <div class="setting-row">
      <div><strong>${escapeHtml(label)}</strong><p>${escapeHtml(hint)}</p></div>
      <select class="compact-input" data-setting="${key}">
        ${options.map(([id, text]) => `<option value="${escapeAttr(id)}" ${id === value ? "selected" : ""}>${escapeHtml(text)}</option>`).join("")}
      </select>
    </div>
  `;
}

function toggleHint(key) {
  const hints = {
    enabled: "Show the active widget on the taskbar.",
    rotationEnabled: "Cycle through selected widgets in the configured order.",
    discordEnabled: "Read selected voice channel users from Discord.",
    discordBackgroundEnabled: "Show the black capsule behind Discord avatars.",
    mediaDarkMode: "Use the modern dark media palette.",
  };
  return hints[key] || "";
}

function mediaDiagnostics() {
  const media = state.mediaStatus || {};
  return `
    <div class="diagnostics-card">
      ${statusRow("Active", formatBool(media.active))}
      ${statusRow("Playing", formatBool(media.playing))}
      ${statusRow("Metadata source", media.metadataSource || "Not available")}
      ${statusRow("Source app", media.sourceApp || "Not available")}
      ${media.error ? statusRow("Error", media.error) : ""}
    </div>
  `;
}

function actionFooter() {
  return `
    <footer class="action-footer">
      <button class="gradient-action" id="save-settings" type="button"><span class="material-symbols-outlined">save</span><span>Save Changes</span></button>
      <button class="secondary-action" id="open-packs" type="button"><span class="material-symbols-outlined">folder_open</span><span>Design Packs</span></button>
      ${inlineStatus()}
    </footer>
  `;
}

function inlineStatus() {
  return `<p id="inline-status" class="inline-status">${escapeHtml(state.status)}</p>`;
}

function desktopTaskbarMarkup() {
  const widget = widgetById(currentPreviewDesign());
  return `
    <div class="desktop-taskbar ${state.previewMode === "rail" ? "rail" : ""}">
      <div class="taskbar-left">
        <span class="material-symbols-outlined">grid_view</span>
        <span class="material-symbols-outlined">folder</span>
        <span class="material-symbols-outlined">language</span>
      </div>
      <div class="rotating-widget" style="--accent:${widget.accent}">
        ${taskbarWidgetCapsule(widget.id)}
      </div>
      <div class="tray">
        <span class="material-symbols-outlined">expand_less</span>
        <span class="material-symbols-outlined">wifi</span>
        <span class="material-symbols-outlined">volume_up</span>
        <time>${new Date().toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" })}</time>
      </div>
    </div>
  `;
}

function renderFloatingTaskbar() {
  const preview = document.getElementById("taskbar-preview");
  preview.innerHTML = `
    <div class="floating-left"><span class="material-symbols-outlined">window</span></div>
    <div class="floating-widgets">
      ${enabledWidgets().slice(0, 3).map((id) => `
        <div class="floating-slot" style="--accent:${widgetById(id).accent}">
          ${taskbarWidgetCapsule(id)}
        </div>
      `).join("")}
      <div class="empty-slot"><span class="material-symbols-outlined">add</span></div>
    </div>
    <div class="floating-tray">
      <span class="material-symbols-outlined">wifi</span>
      <span class="material-symbols-outlined">volume_up</span>
      <time>${new Date().toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" })}</time>
    </div>
  `;
  localizeIcons();
}

function taskbarWidgetCapsule(id) {
  const widget = widgetRuntime(id);
  return `<div class="taskbar-capsule ${id}">${widget.taskbar}</div>`;
}

function renderWidgetModal() {
  let modal = document.getElementById("widget-modal-root");
  if (!state.modalWidgetId) {
    modal?.remove();
    return;
  }
  if (!modal) {
    modal = document.createElement("div");
    modal.id = "widget-modal-root";
    document.body.appendChild(modal);
  }
  modal.innerHTML = widgetSettingsModal(state.modalWidgetId);
  bindWidgetModal();
  localizeIcons();
}

function widgetSettingsModal(id) {
  const catalog = widgetById(id);
  const widget = widgetState(id);
  const wasActive = state.settings.activeDesign;
  state.settings.activeDesign = catalog.id;
  const fields = currentWidgetSettingsFields();
  state.settings.activeDesign = wasActive;
  return `
    <div class="modal-backdrop" data-close-modal></div>
    <section class="widget-modal glass-panel" style="--accent:${catalog.accent}" role="dialog" aria-modal="true" aria-label="${escapeAttr(catalog.title)} settings">
      <header class="modal-head">
        <div class="widget-title-block">
          <div class="widget-icon"><span class="material-symbols-outlined filled">${catalog.icon}</span></div>
          <div>
            <h3>${escapeHtml(catalog.title)}</h3>
            <p>${escapeHtml(catalog.category)} widget settings</p>
          </div>
        </div>
        <button class="icon-button" data-close-modal type="button" aria-label="Close">
          <span class="material-symbols-outlined">close</span>
        </button>
      </header>

      <div class="modal-preview">
        ${widgetRuntime(catalog.id).preview}
      </div>

      <div class="modal-settings">
        ${settingToggle("enabled", "Enable Widget", widget.enabled, "widget")}
        ${rangeSetting("positionPct", "Taskbar Position", "0% is taskbar left, 100% is before the system tray", widget.positionPct ?? 100, 0, 100, "%", "widget")}
        ${rangeSetting("moveX", "Move X", "Fine tune horizontal offset in pixels.", widget.moveX ?? 0, -640, 640, "px", "widget")}
        ${fields}
      </div>

      <footer class="modal-actions">
        <button class="secondary-action" data-open-full-settings="${catalog.id}" type="button">
          <span class="material-symbols-outlined">tune</span>
          <span>Full Settings</span>
        </button>
        <button class="gradient-action" id="save-widget-modal" type="button">
          <span class="material-symbols-outlined">save</span>
          <span>Save</span>
        </button>
      </footer>
    </section>
  `;
}

function runtimeControlPanel() {
  return `
    <section class="runtime-control-card">
      <div>
        <strong>Explorer Integration</strong>
        <p>Load injects TaskbarWidgets widgets into explorer.exe. Unload removes every TaskbarWidgets hook from explorer.exe and stops the loader.</p>
      </div>
      <div class="runtime-actions">
        <button class="secondary-action" data-runtime-action="unload" type="button">
          <span class="material-symbols-outlined">eject</span>
          <span>Unload</span>
        </button>
        <button class="gradient-action" data-runtime-action="load" type="button">
          <span class="material-symbols-outlined">play_arrow</span>
          <span>Load</span>
        </button>
      </div>
      <p id="runtime-control-status"></p>
    </section>
  `;
}

function widgetRuntime(id) {
  if (id === "weather-static") {
    return {
      preview: `<div class="native-preview-stage"><div class="native-widget weather-static"><div class="weather-text"><strong>${escapeHtml(state.settings.weatherCity || "Izmir")}</strong><small>21:24 • 15/07</small></div><strong class="weather-temp">26°</strong><img src="./assets/weather/rain.png" alt="" /></div></div>`,
      taskbar: `<div class="weather-text"><strong>${escapeHtml(state.settings.weatherCity || "Istanbul")}</strong><small>21:24 - Clear</small></div><strong class="weather-temp">26 deg</strong><span class="material-symbols-outlined filled">light_mode</span>`,
    };
  }
  if (id === "discord-voice") {
    return {
      preview: `<div class="native-preview-stage"><div class="native-widget discord-voice"><span class="discord-avatar-frame"><i></i></span><span class="discord-avatar-frame speaking"><i></i></span><span class="discord-avatar-frame"><i></i></span></div></div>`,
      taskbar: `<span class="avatar-mini active"></span><span class="avatar-mini speaking"></span><span class="avatar-mini active"></span>`,
    };
  }
  if (id === "media-player") {
    const media = state.mediaStatus || {};
    const mediaTitle = media.title || "No media";
    const mediaArtist = media.artist || "Open a player";
    const darkMode = state.settings.mediaDarkMode !== false;
    return {
      preview: `<div class="native-preview-stage"><div class="native-widget media-player ${darkMode ? "dark" : "light"}"><img class="native-cover" src="./assets/widgets/media_cover.png" alt="" /><div class="media-copy"><strong>${escapeHtml(mediaTitle)}</strong><small>${escapeHtml(mediaArtist)}</small></div><span class="media-play"><span class="material-symbols-outlined filled">play_arrow</span></span></div></div>`,
      taskbar: `<span class="album-dot"></span><div class="media-copy"><strong>Now Playing</strong><small>System media</small></div><span class="media-play"><span class="material-symbols-outlined filled">play_arrow</span></span>`,
    };
  }
  if (id === "steam-download") {
    return {
      preview: `<div class="native-preview-stage"><div class="native-widget steam-download"><span class="native-cover steam-cover"></span><div class="steam-copy"><strong>Steam Downloads</strong><small>Indirme yok</small></div><div class="steam-metric"><b>--</b><i><em></em></i></div></div></div>`,
      taskbar: `<span class="steam-art mini"></span><div class="steam-copy"><strong>Cyberpunk 2077</strong><small>8 min remaining</small></div><div class="steam-metric"><b>42%</b><i><em></em></i></div>`,
    };
  }
  return {
    preview: `<div class="native-preview-stage"><div class="native-widget codex-status"><div class="codex-line"><strong>Antigravity</strong><span class="native-state-icon">&#xE73E;</span><small>READY</small></div><i class="quota-bar"><em></em></i><div class="codex-metrics"><span>Reset 2h</span><span>Week 61%</span></div></div></div>`,
    taskbar: `<div class="codex-line"><strong>Antigravity</strong><span class="material-symbols-outlined">terminal</span><small>READY</small></div><i class="quota-bar"><em></em></i><div class="codex-metrics"><span>Reset 2h</span><span>Week 61%</span></div>`,
  };
}

function statusRow(label, value) {
  return `<div class="status-row"><span>${escapeHtml(label)}</span><strong>${escapeHtml(value)}</strong></div>`;
}

function bindLibraryPage() {
  document.getElementById("widget-search")?.addEventListener("input", (event) => {
    state.search = event.target.value;
    render();
  });
  bindWidgetButtons();
}

function bindWidgetButtons() {
  document.querySelectorAll("[data-toggle-widget]").forEach((button) => {
    button.onclick = (event) => {
      event.stopPropagation();
      const id = button.dataset.toggleWidget;
      const widget = widgetState(id);
      widget.enabled = !widget.enabled;
      state.settings.activeDesign = id;
      setDirty(true);
      scheduleAutosave();
      render();
    };
  });
  document.querySelectorAll("[data-open-widget]").forEach((element) => {
    element.onclick = (event) => {
      event.preventDefault();
      event.stopPropagation();
      openWidgetModal(element.dataset.openWidget);
    };
  });
  document.querySelectorAll("[data-select-widget]").forEach((button) => {
    button.onclick = () => {
      state.settings.activeDesign = button.dataset.selectWidget;
      state.page = "settings";
      render();
    };
  });
}

function bindRotationPage() {
  bindInputs();
  bindSequenceDragAndDrop();
  document.querySelectorAll("[data-preview-mode]").forEach((button) => {
    button.onclick = () => {
      state.previewMode = button.dataset.previewMode;
      render();
    };
  });
  document.querySelectorAll("[data-move]").forEach((button) => {
    button.onclick = () => {
      const id = button.dataset.move;
      const dir = Number(button.dataset.dir);
      const index = state.settings.rotationDesigns.indexOf(id);
      const next = index + dir;
      if (index >= 0 && next >= 0 && next < state.settings.rotationDesigns.length) {
        [state.settings.rotationDesigns[index], state.settings.rotationDesigns[next]] =
          [state.settings.rotationDesigns[next], state.settings.rotationDesigns[index]];
        setDirty(true);
        scheduleAutosave();
        render();
      }
    };
  });
  document.querySelectorAll("[data-remove-rotation]").forEach((button) => {
    button.onclick = () => {
      const id = button.dataset.removeRotation;
      state.settings.rotationDesigns = state.settings.rotationDesigns.filter((item) => item !== id);
      if (!state.settings.rotationDesigns.length) state.settings.rotationDesigns = [id];
      setDirty(true);
      scheduleAutosave();
      render();
    };
  });
  document.getElementById("enable-all-rotation")?.addEventListener("click", () => {
    state.settings.rotationDesigns = widgetCatalog.map((item) => item.id);
    setDirty(true);
    scheduleAutosave();
    render();
  });
}

function bindSequenceDragAndDrop() {
  document.querySelectorAll("[data-sequence-id]").forEach((item) => {
    item.addEventListener("dragstart", (event) => {
      event.dataTransfer.effectAllowed = "move";
      event.dataTransfer.setData("text/plain", item.dataset.sequenceId);
      item.classList.add("dragging");
    });
    item.addEventListener("dragend", () => {
      item.classList.remove("dragging");
      document.querySelectorAll(".sequence-item.drag-over").forEach((node) => node.classList.remove("drag-over"));
    });
    item.addEventListener("dragover", (event) => {
      event.preventDefault();
      event.dataTransfer.dropEffect = "move";
      item.classList.add("drag-over");
    });
    item.addEventListener("dragleave", () => {
      item.classList.remove("drag-over");
    });
    item.addEventListener("drop", (event) => {
      event.preventDefault();
      const fromId = event.dataTransfer.getData("text/plain");
      const toId = item.dataset.sequenceId;
      item.classList.remove("drag-over");
      reorderRotation(fromId, toId);
    });
  });
}

function reorderRotation(fromId, toId) {
  if (!fromId || !toId || fromId === toId) return;
  const queue = [...state.settings.rotationDesigns];
  const fromIndex = queue.indexOf(fromId);
  const toIndex = queue.indexOf(toId);
  if (fromIndex < 0 || toIndex < 0) return;
  const [moved] = queue.splice(fromIndex, 1);
  queue.splice(toIndex, 0, moved);
  state.settings.rotationDesigns = queue;
  setDirty(true);
  scheduleAutosave();
  render();
}

function openWidgetModal(id) {
  state.modalWidgetId = widgetById(id).id;
  state.settings.activeDesign = state.modalWidgetId;
  render();
}

function closeWidgetModal() {
  state.modalWidgetId = "";
  renderWidgetModal();
}

function bindWidgetModal() {
  bindInputs();
  document.querySelectorAll("[data-close-modal]").forEach((button) => {
    button.onclick = () => closeWidgetModal();
  });
  bindRuntimeControls();
  document.querySelector("[data-open-full-settings]")?.addEventListener("click", (event) => {
    state.settings.activeDesign = event.currentTarget.dataset.openFullSettings;
    state.modalWidgetId = "";
    state.page = "settings";
    render();
  });
  document.getElementById("save-widget-modal")?.addEventListener("click", async () => {
    await saveSettings();
    closeWidgetModal();
    render();
  });
}

async function runRuntimeAction(action) {
  const status = document.getElementById("runtime-control-status");
  const buttons = document.querySelectorAll("[data-runtime-action]");
  const label = action === "unload" ? "Unloading from explorer.exe..." : "Loading into explorer.exe...";
  buttons.forEach((button) => { button.disabled = true; });
  if (status) status.textContent = label;
  try {
    const loaded = await invoke("control_runtime", { action });
    state.settings = mergeSettings(loaded.settings || state.settings);
    state.updateStatus = loaded.updateStatus || state.updateStatus;
    state.mediaStatus = loaded.mediaStatus || state.mediaStatus;
    setStatus(action === "unload" ? "Explorer hooks unloaded" : "Explorer hooks loaded");
    if (status) status.textContent = action === "unload"
      ? "Unloaded from explorer.exe"
      : "Loaded into explorer.exe";
  } catch (error) {
    const message = action === "unload"
      ? `Unload failed: ${error}`
      : `Load failed: ${error}`;
    setStatus(message);
    if (status) status.textContent = message;
  } finally {
    buttons.forEach((button) => { button.disabled = false; });
  }
}

function bindSettingsPage() {
  bindInputs();
  bindWidgetButtons();
  bindRuntimeControls();
  document.getElementById("save-settings")?.addEventListener("click", () => saveSettings());
  document.getElementById("open-packs")?.addEventListener("click", async () => {
    try {
      await invoke("open_widget_libraries");
      setStatus("WidgetLibraries opened");
    } catch (error) {
      setStatus(`Open failed: ${error}`);
    }
  });
  document.getElementById("reset-settings")?.addEventListener("click", async () => {
    try {
      const loaded = await invoke("load_state");
      state.settings = mergeSettings(loaded.settings || {});
      state.updateStatus = loaded.updateStatus || {};
      state.mediaStatus = loaded.mediaStatus || {};
      setDirty(false);
      setStatus("Settings reloaded");
      render();
    } catch (error) {
      setStatus(`Reset failed: ${error}`);
    }
  });
}

function bindRuntimeControls() {
  document.querySelectorAll("[data-runtime-action]").forEach((button) => {
    button.onclick = async () => {
      await runRuntimeAction(button.dataset.runtimeAction);
    };
  });
}

function bindUpdatesPage() {
  document.getElementById("check-updates")?.addEventListener("click", async () => {
    try {
      setStatus("Checking for updates...");
      state.updateStatus = { ...state.updateStatus, state: "checking", message: "Checking GitHub latest release..." };
      render();
      const loaded = await invoke("run_loader_command", { arg: "--check-updates" });
      state.updateStatus = loaded.updateStatus || {};
      setStatus(state.updateStatus.message || "Update check finished");
      render();
      loadReleaseTimeline(true);
    } catch (error) {
      setStatus(`Update check failed: ${error}`);
      await refreshState();
    }
  });
  document.getElementById("install-update")?.addEventListener("click", async () => {
    try {
      setStatus("Installing update...");
      updateInstallRequested = true;
      if (state.updateStatus?.state === "ready" || state.updateStatus?.installerPath) {
        autoLaunchDownloadedInstaller();
        return;
      }

      state.updateStatus = {
        ...state.updateStatus,
        state: "downloading",
        message: "Downloading update...",
        progressPercent: 0,
        downloadedBytes: 0,
      };
      render();
      const loaded = await invoke("start_loader_command", { arg: "--download-update" });
      state.updateStatus = loaded.updateStatus || state.updateStatus;
      startUpdatePolling();
    } catch (error) {
      setStatus(`Update install failed: ${error}`);
      await refreshState();
    }
  });
}

function bindInputs() {
  document.querySelectorAll("[data-setting]").forEach((input) => {
    input.addEventListener("input", () => {
      const key = input.dataset.setting;
      if (input.type === "checkbox") {
        state.settings[key] = input.checked;
      } else if (input.type === "number" || input.type === "range") {
        state.settings[key] = clampNumber(input.value, Number(input.min || 0), Number(input.max || 3600), defaults[key] || 0);
        updateValueLabel("setting", key, state.settings[key], input.dataset.unit || "");
      } else {
        state.settings[key] = input.value;
      }
      setDirty(true);
      scheduleAutosave();
      renderFloatingTaskbar();
    });
  });
  document.querySelectorAll("[data-widget-setting]").forEach((input) => {
    input.addEventListener("input", () => {
      const widget = activeWidget();
      const key = input.dataset.widgetSetting;
      if (input.type === "checkbox") {
        widget[key] = input.checked;
      } else if (input.type === "number" || input.type === "range") {
        widget[key] = clampNumber(input.value, Number(input.min || -640), Number(input.max || 640), widget[key] || 0);
        updateValueLabel("widget", key, widget[key], input.dataset.unit || "");
      } else {
        widget[key] = input.value;
      }
      state.settings.enabled = widget.enabled;
      state.settings.widgetMoveX = widget.moveX;
      state.settings.widgetOffsetPx = Math.max(0, -widget.moveX);
      setDirty(true);
      scheduleAutosave();
      renderFloatingTaskbar();
    });
  });
}

function updateValueLabel(prefix, key, value, unit) {
  const label = document.getElementById(`${prefix}-${key}-value`);
  if (label) label.textContent = `${value}${unit}`;
}

function scheduleAutosave() {
  clearTimeout(autosaveTimer);
  autosaveTimer = setTimeout(() => saveSettings("Applied"), 450);
}

async function saveSettings(successMessage = "Settings saved") {
  try {
    state.settings.rotationDesigns = normalizeRotation(state.settings.rotationDesigns);
    state.settings.widgets = normalizeWidgets(
      state.settings.widgets,
      state.settings.activeDesign,
      state.settings.enabled,
      state.settings.widgetMoveX,
    );
    const widget = activeWidget();
    state.settings.enabled = widget.enabled;
    state.settings.widgetMoveX = widget.moveX;
    state.settings.widgetOffsetPx = Math.max(0, -widget.moveX);
    await invoke("save_settings", { settings: state.settings });
    setDirty(false);
    setStatus(successMessage);
  } catch (error) {
    setStatus(`Save failed: ${error}`);
  }
}

async function refreshState() {
  try {
    const loaded = await invoke("load_state");
    state.updateStatus = loaded.updateStatus || {};
    state.mediaStatus = loaded.mediaStatus || {};
    if (updateInstallRequested && (state.updateStatus?.state === "ready" || state.updateStatus?.installerPath)) {
      autoLaunchDownloadedInstaller();
    }
    render();
    if (!isUpdateBusy(state.updateStatus)) stopUpdatePolling();
  } catch {
    render();
  }
}

async function autoLaunchDownloadedInstaller() {
  if (updateInstallerLaunchInProgress) return;
  updateInstallerLaunchInProgress = true;
  stopUpdatePolling();
  const readyStatus = state.updateStatus || {};
  state.updateStatus = {
    ...readyStatus,
    state: "installing",
    message: "Installer downloaded. Starting setup...",
  };
  setStatus("Starting setup...");
  render();
  try {
    await invoke("launch_downloaded_installer");
    setStatus("Setup started");
  } catch (error) {
    updateInstallerLaunchInProgress = false;
    state.updateStatus = {
      ...readyStatus,
      state: "ready",
      message: `Setup could not be started: ${error}`,
    };
    setStatus(`Setup start failed: ${error}`);
    render();
  }
}

function isUpdateBusy(update) {
  return update?.state === "checking" || update?.state === "downloading";
}

function startUpdatePolling() {
  clearInterval(updatePollTimer);
  updatePollTimer = setInterval(() => {
    refreshState();
  }, 900);
}

function stopUpdatePolling() {
  clearInterval(updatePollTimer);
  updatePollTimer = 0;
}

async function loadReleaseTimeline(force = false) {
  if (state.releaseTimelineState === "loading") return;
  if (state.releaseTimeline.length && !force) return;
  state.releaseTimelineState = "ready";
  state.releaseTimeline = [];
  if (state.page === "updates") render();
}

function startPreviewLoop() {
  clearInterval(previewTimer);
  previewTimer = setInterval(() => {
    const queue = state.settings.rotationEnabled ? state.settings.rotationDesigns : enabledWidgets();
    if (!queue.length) return;
    state.previewIndex = (state.previewIndex + 1) % queue.length;
    if (state.page === "rotation") renderPage();
    renderFloatingTaskbar();
  }, Math.max(5, state.settings.rotationIntervalSecs || 30) * 1000);
}

function setDirty(value) {
  state.dirty = value;
  renderNavigation();
  const pill = document.querySelector(".save-pill");
  if (pill) {
    pill.classList.toggle("dirty", value);
    pill.innerHTML = `<span class="material-symbols-outlined">${value ? "pending" : "check_circle"}</span>${value ? "Unsaved" : "Saved"}`;
    localizeIcons();
  }
}

function setStatus(message) {
  state.status = message || "";
  const status = document.getElementById("inline-status");
  if (status) status.textContent = state.status;
}

function isWidgetEnabled(id) {
  return Boolean(state.settings.widgets.find((widget) => widget.design === id)?.enabled);
}

function formatBool(value) {
  if (value === true) return "Yes";
  if (value === false) return "No";
  return "Unknown";
}

function formatUnixTime(value) {
  const seconds = Number(value);
  if (!Number.isFinite(seconds) || seconds <= 0) return "Not checked";
  return new Date(seconds * 1000).toLocaleString();
}

function formatBytes(value) {
  let bytes = Number(value || 0);
  if (!Number.isFinite(bytes) || bytes < 0) bytes = 0;
  const units = ["B", "KB", "MB", "GB"];
  let index = 0;
  while (bytes >= 1024 && index < units.length - 1) {
    bytes /= 1024;
    index += 1;
  }
  return `${bytes.toFixed(bytes >= 10 || index === 0 ? 0 : 1)} ${units[index]}`;
}

function formatDate(value) {
  if (!value) return "Unknown";
  return new Date(value).toLocaleDateString(undefined, { year: "numeric", month: "short", day: "2-digit" });
}

function escapeHtml(value) {
  return String(value ?? "")
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;");
}

function escapeAttr(value) {
  return escapeHtml(value).replaceAll("'", "&#39;");
}

boot();
