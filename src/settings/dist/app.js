const { invoke } = window.__TAURI__.core;

const widgetPresentation = {
  "codex-status": { icon: "terminal", accent: "#5fd4ff", featured: true },
  "weather-static": { icon: "partly_cloudy_day", accent: "#f59e0b", featured: true },
  "discord-voice": { icon: "forum", accent: "#5865f2" },
  "media-player": { icon: "play_circle", accent: "#1db954" },
  "steam-download": { icon: "download", accent: "#66c0f4", featured: true },
  "system-cpu": { icon: "memory", accent: "#60a5fa", featured: true },
  "system-storage": { icon: "hard_drive", accent: "#34d399" },
  "system-network": { icon: "swap_vert", accent: "#22d3ee", featured: true },
  "system-memory": { icon: "developer_board", accent: "#c084fc" },
};

let widgetCatalog = (window.TASKBAR_WIDGET_CATALOG || []).map((manifest) => ({
  ...manifest,
  title: manifest.displayName,
  authorName: manifest.authorName || manifest.author?.name || "Taskbar Widgets",
  authorWebsite: manifest.authorWebsite || manifest.author?.website || "",
  ...(widgetPresentation[manifest.id] || { icon: "widgets", accent: "#5fd4ff" }),
}));

const defaultWidgets = widgetCatalog.map((widget, index) => ({
  id: widget.id,
  design: widget.id,
  enabled: widget.id === "codex-status",
  moveX: 0,
  positionPct: 100,
  order: index,
  settings: Object.fromEntries((widget.settings || []).map((setting) => [setting.key, setting.default])),
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
    title: "Installed Widgets",
    description: "Manage built-in and local community widgets detected on this computer.",
  },
  explore: {
    title: "Explore",
    description: "Discover permission-reviewed widgets from the PFC remote library.",
  },
  developer: {
    title: "Developer",
    description: "Install folder widgets, inspect validation results, and use twdev for packaging.",
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
  systemSources: { disks: [], interfaces: [] },
  runtimeCatalog: { widgets: [] },
  communityWidgetsDir: "",
  releaseTimeline: [],
  releaseTimelineState: "idle",
  search: "",
  dirty: false,
  status: "",
  previewMode: "bottom",
  previewIndex: 0,
  modalWidgetId: "",
  installPreview: null,
  installSource: "",
  installError: "",
  installEnable: true,
  remoteLibrary: [],
  remoteLibraryState: "idle",
  remoteLibraryError: "",
  remoteSearch: "",
  communityLastCheckedAt: 0,
  communityUpdates: [],
};

let autosaveTimer = 0;
let previewTimer = 0;
let updatePollTimer = 0;
let updateInstallerLaunchInProgress = false;
let updateInstallRequested = false;
let settingsRequestTimer = 0;
let widgetPositionSyncTimer = 0;
let widgetPositionSyncInProgress = false;
let communityUpdateTimer = 0;
const locallyEditedWidgetPositions = new Set();

function widgetById(id) {
  return widgetCatalog.find((item) => item.id === id) || widgetCatalog[0];
}

function isKnownWidget(id) {
  return widgetCatalog.some((item) => item.id === id);
}

function applyRuntimeCatalog(runtimeCatalog) {
  state.runtimeCatalog = runtimeCatalog && Array.isArray(runtimeCatalog.widgets)
    ? runtimeCatalog
    : { widgets: [] };
  for (const manifest of state.runtimeCatalog.widgets.filter((item) => item.valid && item.renderer === "declarative")) {
    const existingIndex = widgetCatalog.findIndex((item) => item.id === manifest.id);
    const widget = {
      ...(existingIndex >= 0 ? widgetCatalog[existingIndex] : {}),
      ...manifest,
      title: manifest.displayName || manifest.id,
      category: manifest.category || "Community",
      icon: "extension",
      accent: "#5fd4ff",
      local: true,
      authorName: manifest.authorName || "Unknown author",
      authorWebsite: manifest.authorWebsite || "",
    };
    if (existingIndex >= 0) {
      widgetCatalog[existingIndex] = widget;
      continue;
    }
    widgetCatalog.push(widget);
    const defaultsForWidget = Object.fromEntries((manifest.settings || []).map((setting) => [setting.key, setting.default]));
    defaultWidgets.push({
      id: manifest.id,
      design: manifest.id,
      enabled: false,
      moveX: 0,
      positionPct: 100,
      order: defaultWidgets.length,
      settings: defaultsForWidget,
    });
    if (!defaults.rotationDesigns.includes(manifest.id)) defaults.rotationDesigns.push(manifest.id);
  }
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
    const instanceId = String(item.instanceId || item.id || design);
    if (result.some((widget) => widget.id === instanceId)) continue;
    result.push({
      id: instanceId,
      design,
      enabled: isKnownWidget(design) && Boolean(item.enabled),
      moveX: clampNumber(item.moveX ?? item.widgetMoveX ?? 0, -640, 640, 0),
      positionPct: clampNumber(item.positionPct ?? 100, 0, 100, 100),
      order: clampNumber(item.order ?? result.length, 0, 1000, result.length),
      settings: {
        ...(defaultWidgets.find((widget) => widget.design === design)?.settings || {}),
        ...(item.settings || {}),
      },
    });
  }

  for (const widget of defaultWidgets) {
    if (!result.some((item) => item.design === widget.design)) {
      result.push({ ...widget, settings: { ...widget.settings }, enabled: false, moveX: 0, positionPct: 100, order: result.length });
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
      settings: { ...(defaultWidgets.find((item) => item.design === design)?.settings || {}) },
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
  const number = Number.parseFloat(value);
  if (!Number.isFinite(number)) return fallback;
  return Math.min(max, Math.max(min, number));
}

async function boot() {
  try {
    const loaded = await invoke("load_state");
    state.appDir = loaded.appDir || "";
    state.communityWidgetsDir = loaded.communityWidgetsDir || "";
    applyRuntimeCatalog(loaded.widgetCatalog);
    state.settings = mergeSettings(loaded.settings || {});
    state.updateStatus = loaded.updateStatus || {};
    state.mediaStatus = loaded.mediaStatus || {};
    state.systemSources = loaded.systemSources || { disks: [], interfaces: [] };
    applyCommunityUpdateState(loaded.communityUpdateState);
  } catch (error) {
    state.status = `Load failed: ${error}`;
  }
  render();
  await consumeSettingsOpenRequest();
  await consumeWidgetInstallRequest();
  settingsRequestTimer = setInterval(consumeSettingsOpenRequest, 500);
  setInterval(consumeWidgetInstallRequest, 700);
  widgetPositionSyncTimer = setInterval(syncWidgetPositions, 600);
  loadReleaseTimeline();
  startPreviewLoop();
  if (isUpdateBusy(state.updateStatus)) startUpdatePolling();
  bindCommunityDropTarget();
  setTimeout(() => loadRemoteLibrary(false), 1200);
  communityUpdateTimer = setInterval(() => loadRemoteLibrary(true), 30 * 60 * 1000);
}

async function bindCommunityDropTarget() {
  try {
    const webview = window.__TAURI__?.webview?.getCurrentWebview?.();
    if (!webview) return;
    await webview.onDragDropEvent(async (event) => {
      if (event.payload?.type !== "drop") return;
      for (const source of event.payload.paths || []) {
        await openWidgetInstall(source);
      }
    });
  } catch {
    // Drag/drop is optional on older embedded Tauri webviews; picker install remains available.
  }
}

async function syncWidgetPositions() {
  if (widgetPositionSyncInProgress) return;
  widgetPositionSyncInProgress = true;
  try {
    const loaded = await invoke("load_state");
    const updatesChanged = applyCommunityUpdateState(loaded.communityUpdateState);
    const incoming = normalizeWidgets(
      loaded.settings?.widgets,
      state.settings.activeDesign,
      state.settings.enabled,
      state.settings.widgetMoveX,
    );
    let changed = false;
    for (const current of state.settings.widgets) {
      if (locallyEditedWidgetPositions.has(current.design)) continue;
      const saved = incoming.find((widget) => widget.design === current.design);
      if (!saved || (saved.positionPct === current.positionPct && saved.moveX === current.moveX)) continue;
      current.positionPct = saved.positionPct;
      current.moveX = saved.moveX;
      changed = true;
    }
    if (!changed && !updatesChanged) return;

    if (changed) {
      const active = activeWidget();
      state.settings.widgetMoveX = active.moveX;
      state.settings.widgetOffsetPx = Math.max(0, -active.moveX);
      document.querySelectorAll('[data-widget-setting="positionPct"]').forEach((input) => {
        input.value = active.positionPct;
      });
      document.querySelectorAll('[data-widget-setting="moveX"]').forEach((input) => {
        input.value = active.moveX;
      });
      updateValueLabel("widget", "positionPct", active.positionPct, "%");
      updateValueLabel("widget", "moveX", active.moveX, "px");
      renderFloatingTaskbar();
      setStatus("Taskbar position updated");
    }
    if (updatesChanged && state.page === "library") renderPage();
  } catch {
    // The loader may be replacing config.json atomically; retry on the next tick.
  } finally {
    widgetPositionSyncInProgress = false;
  }
}

async function consumeSettingsOpenRequest() {
  try {
    const widgetId = await invoke("consume_settings_open_request");
    if (!widgetId || !isKnownWidget(widgetId)) return;
    state.settings.activeDesign = widgetId;
    state.modalWidgetId = widgetId;
    state.page = "settings";
    render();
  } catch {
    // A missing or concurrently consumed request is expected.
  }
}

async function consumeWidgetInstallRequest() {
  try {
    const source = await invoke("consume_widget_install_request");
    if (source) await openWidgetInstall(source);
  } catch {
    // A missing or concurrently consumed request is expected.
  }
}

function render() {
  renderNavigation();
  renderPage();
  renderFloatingTaskbar();
  renderWidgetModal();
  renderInstallModal();
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
    volume_up: "◖", window: "⊞", eject: "■", play_arrow: "▶", light_mode: "☀", shield: "◆", delete: "×"
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
      if (state.page === "explore" && state.remoteLibraryState === "idle") {
        loadRemoteLibrary();
      }
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
  } else if (state.page === "explore") {
    page.innerHTML = explorePage();
    bindExplorePage();
  } else if (state.page === "developer") {
    page.innerHTML = developerPage();
    bindDeveloperPage();
  } else {
    page.innerHTML = libraryPage();
    bindLibraryPage();
  }
  localizeIcons();
}

function explorePage() {
  const query = state.remoteSearch.trim().toLowerCase();
  const widgets = state.remoteLibrary.filter((item) => !query ||
    [item.displayName, item.description, item.category, item.author?.name, item.id]
      .join(" ").toLowerCase().includes(query));
  const body = state.remoteLibraryState === "loading"
    ? `<div class="library-empty"><strong>Loading community library…</strong><span>Reading index.json and widget details.</span></div>`
    : state.remoteLibraryState === "error"
      ? `<section class="glass-panel channel-card library-unavailable"><span class="status-chip">Library offline</span><h3>The remote library is not ready yet</h3><p>${escapeHtml(state.remoteLibraryError || "The server did not return a valid index.json.")}</p><button class="secondary-action" id="retry-remote-library" type="button"><span class="material-symbols-outlined">sync</span><span>Try Again</span></button></section>`
      : widgets.length
        ? `<section class="remote-widget-grid">${widgets.map(remoteWidgetCard).join("")}</section>`
        : `<div class="library-empty"><strong>No community widgets found</strong><span>${query ? "Try another search." : "The catalog is valid but currently empty."}</span></div>`;
  return `${pageHeader("explore")}
    <section class="library-toolbar">
      <div class="search-box"><span class="material-symbols-outlined">search</span><input id="remote-widget-search" value="${escapeAttr(state.remoteSearch)}" placeholder="Search community widgets…" /></div>
      <button class="secondary-action" id="refresh-remote-library" type="button"><span class="material-symbols-outlined">sync</span><span>Refresh</span></button>
    </section>
    ${body}
    ${inlineStatus()}`;
}

function permissionEntries(permissions) {
  return Object.entries(permissions || {}).filter(([, value]) => Array.isArray(value) ? value.length : Boolean(value));
}

function permissionLabel(key, value) {
  const names = { network: "Network", systemMetrics: "System metrics", openExternal: "Open links", storage: "Private storage" };
  const suffix = Array.isArray(value) ? `: ${value.join(", ")}` : "";
  return `${names[key] || key}${suffix}`;
}

function compareVersions(left, right) {
  const parse = (value) => {
    const parts = String(value || "").split(".");
    if (parts.length < 3 || parts.length > 4 || parts.some((part) => !/^\d+$/.test(part))) return null;
    return parts.map(Number).concat([0, 0, 0, 0]).slice(0, 4);
  };
  const a = parse(left);
  const b = parse(right);
  if (!a || !b) return 0;
  for (let index = 0; index < 4; index += 1) {
    if (a[index] !== b[index]) return a[index] > b[index] ? 1 : -1;
  }
  return 0;
}

function remoteUpdateFor(widgetId) {
  const installed = widgetCatalog.find((item) => item.id === widgetId && item.local);
  const remote = state.remoteLibrary.find((item) => item.id === widgetId);
  if (installed && remote && compareVersions(remote.version, installed.version) > 0) return remote;
  const background = state.communityUpdates.find((item) => item.widgetId === widgetId);
  return installed && background && compareVersions(background.availableVersion, installed.version) > 0
    ? { id: widgetId, version: background.availableVersion, displayName: background.displayName || installed.title }
    : null;
}

function applyCommunityUpdateState(updateState) {
  if (!updateState || updateState.schemaVersion !== 1) return false;
  const checkedAt = Number(updateState.checkedAtUnix || 0) * 1000;
  if (checkedAt && checkedAt <= state.communityLastCheckedAt) return false;
  state.communityLastCheckedAt = checkedAt || state.communityLastCheckedAt;
  state.communityUpdates = Array.isArray(updateState.updates) ? updateState.updates : [];
  return true;
}

function remoteWidgetCard(widget) {
  const installedWidget = widgetCatalog.find((item) => item.id === widget.id);
  const installed = Boolean(installedWidget);
  const updateAvailable = installedWidget?.local && compareVersions(widget.version, installedWidget.version) > 0;
  const permissions = permissionEntries(widget.permissions);
  return `<article class="glass-panel remote-widget-card" style="--accent:#5fd4ff">
    <div class="remote-widget-head"><span class="widget-icon"><span class="material-symbols-outlined">extension</span></span><span class="status-chip">${escapeHtml(widget.category || "Community")}</span></div>
    <h3>${escapeHtml(widget.displayName)}</h3>
    <p>${escapeHtml(widget.description)}</p>
    <div class="remote-author">By <strong>${escapeHtml(widget.author?.name || "Unknown author")}</strong> · v${escapeHtml(widget.version)}</div>
    <div class="permission-chips">${permissions.length ? permissions.map(([key, value]) => `<span>${escapeHtml(permissionLabel(key, value))}</span>`).join("") : "<span>No additional permissions</span>"}</div>
    ${updateAvailable ? `<div class="update-version-line"><strong>Update available</strong><span>v${escapeHtml(installedWidget.version)} → v${escapeHtml(widget.version)}</span></div>` : ""}
    <button class="${installed && !updateAvailable ? "secondary-action" : "gradient-action"}" data-download-remote="${escapeAttr(widget.id)}" type="button" ${installed && !updateAvailable ? "disabled" : ""}>
      <span class="material-symbols-outlined">${updateAvailable ? "sync" : installed ? "check_circle" : "download"}</span><span>${updateAvailable ? "Review & Update" : installed ? "Installed" : "Review & Install"}</span>
    </button>
  </article>`;
}

async function loadRemoteLibrary(force = false) {
  if (!force && state.remoteLibraryState === "loading") return;
  state.remoteLibraryState = "loading";
  state.remoteLibraryError = "";
  if (state.page === "explore") renderPage();
  try {
    state.remoteLibrary = await invoke("fetch_remote_library");
    state.remoteLibraryState = "ready";
    state.communityLastCheckedAt = Date.now();
    state.communityUpdates = state.remoteLibrary.map((remote) => {
      const installed = widgetCatalog.find((item) => item.id === remote.id && item.local);
      return installed && compareVersions(remote.version, installed.version) > 0
        ? { widgetId: remote.id, installedVersion: installed.version, availableVersion: remote.version, displayName: remote.displayName }
        : null;
    }).filter(Boolean);
  } catch (error) {
    state.remoteLibrary = [];
    state.remoteLibraryState = "error";
    state.remoteLibraryError = String(error);
  }
  if (state.page === "explore" || state.page === "library") renderPage();
}

function bindExplorePage() {
  document.getElementById("remote-widget-search")?.addEventListener("input", (event) => {
    state.remoteSearch = event.target.value;
    renderPage();
  });
  document.getElementById("refresh-remote-library")?.addEventListener("click", () => loadRemoteLibrary(true));
  document.getElementById("retry-remote-library")?.addEventListener("click", () => loadRemoteLibrary(true));
  document.querySelectorAll("[data-download-remote]").forEach((button) => {
    button.addEventListener("click", () => downloadAndReviewWidget(button.dataset.downloadRemote, button));
  });
}

async function downloadAndReviewWidget(widgetId, button = null) {
  if (button) button.disabled = true;
  setStatus("Downloading and verifying widget package…");
  try {
    const source = await invoke("download_remote_widget", { widgetId });
    setStatus("Package verified");
    await openWidgetInstall(source);
  } catch (error) {
    setStatus(`Download failed: ${error}`);
    if (button) button.disabled = false;
  }
}

async function removeCommunityWidget(widgetId, ask = true) {
  const manifest = widgetCatalog.find((item) => item.id === widgetId) ||
    (state.runtimeCatalog.widgets || []).find((item) => item.id === widgetId && !item.trusted);
  if (!manifest || manifest.trusted || (!manifest.local && manifest.renderer !== "declarative")) return;
  const title = manifest.title || manifest.displayName || widgetId;
  if (ask && !window.confirm(`Remove ${title}? All instances and saved settings for this widget will be removed.`)) return;
  try {
    await invoke("remove_community_widget", { widgetId });
    state.settings.widgets = state.settings.widgets.filter((item) => item.design !== widgetId);
    state.settings.rotationDesigns = state.settings.rotationDesigns.filter((id) => id !== widgetId);
    state.communityUpdates = state.communityUpdates.filter((item) => item.widgetId !== widgetId);
    state.runtimeCatalog.widgets = (state.runtimeCatalog.widgets || []).filter((item) => item.id !== widgetId);
    widgetCatalog = widgetCatalog.filter((item) => item.id !== widgetId);
    const defaultIndex = defaultWidgets.findIndex((item) => item.design === widgetId);
    if (defaultIndex >= 0) defaultWidgets.splice(defaultIndex, 1);
    defaults.rotationDesigns = defaults.rotationDesigns.filter((id) => id !== widgetId);
    if (state.settings.activeDesign === widgetId) state.settings.activeDesign = "codex-status";
    if (state.modalWidgetId === widgetId) state.modalWidgetId = "";
    await saveSettings(`${title} removed`);
    render();
  } catch (error) {
    setStatus(`Remove failed: ${error}`);
  }
}

function developerPage() {
  const entries = state.runtimeCatalog.widgets || [];
  const community = entries.filter((item) => !item.trusted);
  return `${pageHeader("developer")}
    <section class="glass-panel channel-card">
      <h3>Community widget folder</h3>
      <p><code>${escapeHtml(state.communityWidgetsDir || "%LocalAppData%\\TaskbarWidgets\\CommunityWidgets")}</code></p>
      <div class="update-actions">
        <button class="gradient-action" id="open-community-folder" type="button"><span class="material-symbols-outlined">folder_open</span><span>Open Folder</span></button>
        <button class="secondary-action" id="install-community-folder" type="button"><span class="material-symbols-outlined">add</span><span>Install Folder</span></button>
        <button class="secondary-action" id="install-community-package" type="button"><span class="material-symbols-outlined">download</span><span>Install .twidget</span></button>
        <button class="secondary-action" id="reload-community-catalog" type="button"><span class="material-symbols-outlined">sync</span><span>Reload</span></button>
      </div>
    </section>
    <section class="widget-library-list" aria-label="Community validation results">
      ${community.length ? community.map((item) => `<article class="widget-library-row ${item.valid ? "enabled" : ""}">
        <div class="widget-icon"><span class="material-symbols-outlined">${item.valid ? "check_circle" : "warning"}</span></div>
        <div class="widget-library-copy"><div class="widget-library-title"><h3>${escapeHtml(item.displayName || item.id)}</h3><span>${item.valid ? "Local / Unverified" : "Rejected"}</span></div>
        <p>${escapeHtml(item.error || (item.valid ? `${item.id} · ${item.version}` : "Validation failed"))}</p></div>
        <button class="icon-button" data-remove-community="${escapeAttr(item.id)}" type="button" title="Remove local widget"><span class="material-symbols-outlined">close</span></button>
      </article>`).join("") : `<div class="library-empty"><strong>No local widgets</strong><span>Copy a widget folder here or run twdev dev.</span></div>`}
    </section>
    <section class="glass-panel channel-card"><h3>Developer CLI</h3><p><code>twdev init com.example.clock</code><br/><code>twdev validate ./com.example.clock</code><br/><code>twdev dev ./com.example.clock</code><br/><code>twdev pack ./com.example.clock</code></p></section>
    ${inlineStatus()}`;
}

function bindDeveloperPage() {
  document.getElementById("open-community-folder")?.addEventListener("click", async () => {
    try { await invoke("open_widget_libraries"); setStatus("Community folder opened"); }
    catch (error) { setStatus(`Open failed: ${error}`); }
  });
  const installFromPicker = async (directory) => {
    try {
      const source = await window.__TAURI__.dialog.open(directory
        ? { directory: true, multiple: false }
        : { multiple: false, filters: [{ name: "Taskbar Widget", extensions: ["twidget"] }] });
      if (!source) return;
      await openWidgetInstall(source);
    } catch (error) { setStatus(`Install failed: ${error}`); }
  };
  document.getElementById("install-community-folder")?.addEventListener("click", () => installFromPicker(true));
  document.getElementById("install-community-package")?.addEventListener("click", () => installFromPicker(false));
  document.querySelectorAll("[data-remove-community]").forEach((button) => {
    button.addEventListener("click", async () => {
      const id = button.dataset.removeCommunity;
      await removeCommunityWidget(id, true);
    });
  });
  document.getElementById("reload-community-catalog")?.addEventListener("click", async () => {
    try {
      const loaded = await invoke("load_state");
      applyRuntimeCatalog(loaded.widgetCatalog);
      state.runtimeCatalog = loaded.widgetCatalog || { widgets: [] };
      setStatus("Catalog reloaded"); render();
    } catch (error) { setStatus(`Reload failed: ${error}`); }
  });
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
    return [widget.title, widget.category, widget.description, widget.authorName].join(" ").toLowerCase().includes(q);
  });
  return `
    ${pageHeader("library")}
    <section class="library-toolbar">
      <div class="search-box">
        <span class="material-symbols-outlined">search</span>
        <input id="widget-search" value="${escapeAttr(state.search)}" placeholder="Search widgets..." />
      </div>
      <div class="library-toolbar-actions">
        <span class="library-count">${filtered.length} widget${filtered.length === 1 ? "" : "s"}${state.communityLastCheckedAt ? ` · checked ${new Date(state.communityLastCheckedAt).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" })}` : ""}</span>
        <button class="secondary-action" id="check-community-updates" type="button"><span class="material-symbols-outlined">sync</span><span>Check Updates</span></button>
      </div>
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
  const update = remoteUpdateFor(widget.id);
  return `
    <article class="widget-library-row ${enabled ? "enabled" : ""}" style="--accent:${widget.accent}" data-open-widget="${widget.id}" role="button" tabindex="0" aria-label="Open ${escapeAttr(widget.title)} settings">
      <button class="native-preview-button" type="button" aria-label="Open ${escapeAttr(widget.title)} settings">
        ${runtime.preview}
      </button>
      <div class="widget-library-copy">
        <div class="widget-library-title">
          <span class="widget-accent-dot"></span>
          <h3>${escapeHtml(widget.title)}</h3>
          <span>${escapeHtml(widget.category)}</span>
        </div>
        <p>${escapeHtml(widget.description)}</p>
        <small class="installed-author">By ${escapeHtml(widget.authorName || "Taskbar Widgets")}</small>
        ${update ? `<div class="installed-update"><strong>Update available</strong><span>v${escapeHtml(widget.version)} → v${escapeHtml(update.version)}</span></div>` : ""}
      </div>
      <div class="installed-widget-actions">
        ${update ? `<button class="secondary-action compact-action" data-update-community="${escapeAttr(widget.id)}" type="button"><span class="material-symbols-outlined">sync</span><span>Update</span></button>` : ""}
        ${widget.local ? `<button class="icon-button danger-action" data-remove-installed="${escapeAttr(widget.id)}" type="button" title="Remove widget"><span class="material-symbols-outlined">delete</span></button>` : ""}
        ${toggleButton(widget.id, true)}
      </div>
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
        ${systemMeterTabs()}
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

function systemMeterTabs() {
  const ids = ["system-cpu", "system-storage", "system-network", "system-memory"];
  const widgets = state.settings.widgets.filter((widget) => ids.includes(widget.design)).sort((a, b) => a.order - b.order);
  return `<div class="system-meter-tabs" aria-label="System meter order">
    ${widgets.map((widget) => `<button draggable="true" data-system-meter-tab="${widget.design}" class="${state.settings.activeDesign === widget.design ? "active" : ""}" type="button">
      <span class="system-tab-check"><input type="checkbox" data-system-tab-enabled="${widget.design}" ${widget.enabled ? "checked" : ""} aria-label="Enable ${escapeAttr(widgetById(widget.design).title)}" /></span>
      <span>${escapeHtml(widgetById(widget.design).title)}</span><i></i>
    </button>`).join("")}
  </div>`;
}

function currentWidgetSettingsFields() {
  const id = state.settings.activeDesign;
  const manifest = widgetById(id);
  if (manifest?.local) return communityWidgetSettingsFields(manifest);
  if (id.startsWith("system-")) return systemWidgetSettingsFields(id);
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

function communityWidgetSettingsFields(manifest) {
  const widget = activeWidget();
  const values = widget.settings || (widget.settings = {});
  const permissions = Object.entries(manifest.permissions || {}).filter(([, value]) => Array.isArray(value) ? value.length : Boolean(value));
  const fields = (manifest.settings || []).map((setting) => {
    const value = values[setting.key] ?? setting.default;
    if (setting.type === "boolean") return instanceToggle(setting.key, setting.label || setting.key, setting.description || "Widget setting", Boolean(value));
    if (setting.type === "number") return instanceNumberSetting(setting.key, setting.label || setting.key, setting.description || "Widget setting", value, setting.minimum ?? 0, setting.maximum ?? 1000000, setting.step ?? 1);
    if (setting.type === "select" && Array.isArray(setting.options)) return instanceSelectSetting(setting.key, setting.label || setting.key, setting.description || "Widget setting", value, setting.options.map((option) => [option.value, option.label || option.value]));
    return `<div class="setting-block"><div class="setting-head"><div><strong>${escapeHtml(setting.label || setting.key)}</strong><p>${escapeHtml(setting.description || "Widget setting")}</p></div></div><input class="text-input" type="${setting.type === "secret" ? "password" : "text"}" data-instance-setting="${escapeAttr(setting.key)}" value="${escapeAttr(value ?? "")}" /></div>`;
  });
  fields.unshift(`<div class="setting-row"><div><strong>Trust</strong><p>Local folders are not reviewed by the Taskbar Widgets registry.</p></div><span class="status-chip">Local / Unverified</span></div>`);
  if (permissions.length) fields.unshift(`<div class="setting-block"><div class="setting-head"><div><strong>Requested permissions</strong><p>${escapeHtml(permissions.map(([key, value]) => `${key}: ${Array.isArray(value) ? value.join(", ") : value}`).join(" · "))}</p></div></div></div>`);
  return fields.join("");
}

function systemWidgetSettingsFields(id) {
  const widget = activeWidget();
  const values = widget.settings || (widget.settings = {});
  const defaultMode = id === "system-cpu" ? "bar" : id === "system-memory" ? "pie" : "text";
  const mode = values.displayMode || defaultMode;
  const fields = [
    instanceRadioSetting("displayMode", "Type", mode, [["bar", "Bar"], ["pie", "Pie"], ["text", "Text"]]),
    instanceRangeSetting("refreshSeconds", "Refresh Rate", "Sampling interval used by this meter.", Number(values.refreshSeconds ?? 3), 0.1, 10, 0.1, " Seconds"),
  ];
  if (mode !== "text") fields.push(instanceColorSetting("outlineColor", "Outline", values.outlineColor || (id === "system-memory" ? "systemAccent" : "#FFFFFFFF")));
  if (id === "system-cpu") {
    fields.push(instanceToggle("showIndividualCores", "Show Individual Cores", "Draw one meter for each logical core.", values.showIndividualCores !== false));
    if (values.showIndividualCores !== false) fields.push(instanceToggle("combineLogicalCores", "Combine Logical Cores", "Combine adjacent logical cores into physical-core pairs.", values.combineLogicalCores === true));
    fields.push(instanceToggle("separateUtilization", "Separate User / Privileged Utilization", "Show user and privileged activity as separate colors.", values.separateUtilization !== false));
    if (values.separateUtilization !== false) {
      fields.push(instanceColorSetting("systemColor", "System", values.systemColor || "#FFFFFFFF"));
      fields.push(instanceColorSetting("userColor", "User", values.userColor || "systemAccent"));
    } else {
      fields.push(instanceColorSetting("cpuColor", "CPU", values.cpuColor || "systemAccent"));
    }
  }
  if (id === "system-storage") {
    fields.push(instanceColorSetting("readColor", "Read", values.readColor || "#FFFFFFFF"));
    fields.push(instanceColorSetting("writeColor", "Write", values.writeColor || "#FFFFFFFF"));
    fields.push(instanceSelectSetting("diskId", "Disk", "Use all physical disks or a counter discovered by the loader.", values.diskId || "_Total", sourceOptions(values.diskId, "_Total", "All disks", state.systemSources.disks)));
  }
  if (id === "system-network") {
    fields.push(instanceColorSetting("sendColor", "Send", values.sendColor || "systemAccent"));
    fields.push(instanceColorSetting("receiveColor", "Receive", values.receiveColor || "systemAccent"));
    fields.push(instanceSelectSetting("interfaceId", "Network Interface", "All sums every active interface.", values.interfaceId || "all", sourceOptions(values.interfaceId, "all", "All interfaces", state.systemSources.interfaces)));
    if (mode !== "text") {
      fields.push(instanceToggle("autoBandwidth", "Automatically Detect Bandwidth", "Use the link speed reported by Windows.", values.autoBandwidth !== false));
      if (values.autoBandwidth === false) fields.push(instanceNumberSetting("bandwidthKiloBytes", "Bandwidth (KiloBytes)", "Manual capacity used for bar and pie utilization.", values.bandwidthKiloBytes || 125000, 1, 1000000000, 1));
    }
  }
  if (id === "system-memory") {
    fields.push(instanceColorSetting("usedColor", "Memory Used", values.usedColor || "systemAccent"));
  }
  return fields.join("");
}

function sourceOptions(current, fallbackId, fallbackName, source) {
  const options = [[fallbackId, fallbackName]];
  for (const item of source || []) {
    if (!options.some(([id]) => id === item.id)) options.push([item.id, item.name || item.id]);
  }
  if (current && !options.some(([id]) => id === current)) options.push([current, `${current} (unavailable)`]);
  return options;
}

function updatesPage() {
  const update = state.updateStatus || {};
  const busy = isUpdateBusy(update);
  const downloading = update.state === "downloading";
  const installing = update.state === "installing" || updateInstallerLaunchInProgress;
  const current = update.currentVersion || "0.4.2";
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

function instanceSelectSetting(key, label, hint, value, options) {
  return `
    <div class="setting-row">
      <div><strong>${escapeHtml(label)}</strong><p>${escapeHtml(hint)}</p></div>
      <select class="compact-input" data-instance-setting="${escapeAttr(key)}">
        ${options.map(([id, text]) => `<option value="${escapeAttr(id)}" ${String(id) === String(value) ? "selected" : ""}>${escapeHtml(text)}</option>`).join("")}
      </select>
    </div>
  `;
}

function instanceColorSetting(key, label, value) {
  const color = /^#[0-9a-f]{6}$/i.test(String(value || "")) ? value : /^#[0-9a-f]{8}$/i.test(String(value || "")) ? `#${String(value).slice(3)}` : "#2986cc";
  return `
    <div class="setting-row">
      <div><strong>${escapeHtml(label)}</strong><p>Hex color or systemAccent.</p></div>
      <div class="meter-color-control"><input class="color-input" type="color" data-color-target="${escapeAttr(key)}" value="${escapeAttr(color)}" /><input class="compact-input color-text" data-instance-setting="${escapeAttr(key)}" value="${escapeAttr(value || "systemAccent")}" /></div>
    </div>
  `;
}

function instanceRadioSetting(key, label, value, options) {
  return `<div class="setting-block"><div class="setting-head"><div><strong>${escapeHtml(label)}</strong></div></div><div class="meter-type-radios">${options.map(([id, text]) => `<label><input type="radio" name="meter-${escapeAttr(key)}" data-instance-setting="${escapeAttr(key)}" value="${id}" ${id === value ? "checked" : ""}/><span>${text}</span></label>`).join("")}</div></div>`;
}

function instanceRangeSetting(key, label, hint, value, min, max, step, unit) {
  return `<div class="setting-block"><div class="setting-head"><div><strong>${escapeHtml(label)}</strong><p>${escapeHtml(hint)}</p></div><span id="instance-${key}-value">${Number(value).toFixed(1)}${unit}</span></div><input type="range" min="${min}" max="${max}" step="${step}" data-instance-setting="${key}" data-unit="${escapeAttr(unit)}" value="${escapeAttr(value)}" /></div>`;
}

function instanceNumberSetting(key, label, hint, value, min, max, step) {
  return `<div class="setting-row"><div><strong>${escapeHtml(label)}</strong><p>${escapeHtml(hint)}</p></div><input class="compact-input" type="number" min="${min}" max="${max}" step="${step}" data-instance-setting="${key}" value="${escapeAttr(value)}" /></div>`;
}

function instanceToggle(key, label, hint, checked) {
  return `
    <div class="setting-row">
      <div><strong>${escapeHtml(label)}</strong><p>${escapeHtml(hint)}</p></div>
      <label class="win-toggle"><input type="checkbox" data-instance-setting="${escapeAttr(key)}" ${checked ? "checked" : ""} /><span><i></i></span></label>
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

async function openWidgetInstall(source) {
  state.installSource = String(source || "");
  state.installPreview = { loading: true };
  state.installError = "";
  state.installEnable = true;
  renderInstallModal();
  try {
    state.installPreview = await invoke("inspect_community_widget", { source: state.installSource });
    if (state.installPreview.alreadyInstalled) {
      state.installEnable = isWidgetEnabled(state.installPreview.id);
    }
  } catch (error) {
    state.installPreview = null;
    state.installError = String(error);
  }
  renderInstallModal();
}

function renderInstallModal() {
  let root = document.getElementById("widget-install-modal-root");
  if (!state.installPreview && !state.installError) {
    root?.remove();
    return;
  }
  if (!root) {
    root = document.createElement("div");
    root.id = "widget-install-modal-root";
    document.body.appendChild(root);
  }
  if (state.installPreview?.loading) {
    root.innerHTML = `<div class="modal-backdrop"></div><section class="widget-modal install-modal glass-panel" role="dialog" aria-modal="true"><div class="install-loading"><span class="material-symbols-outlined">sync</span><strong>Inspecting widget package…</strong><p>Checking package paths, manifest, author and requested permissions.</p></div></section>`;
    localizeIcons();
    return;
  }
  if (state.installError) {
    root.innerHTML = `<div class="modal-backdrop" data-close-install></div><section class="widget-modal install-modal glass-panel" role="dialog" aria-modal="true"><header class="modal-head"><div><h3>Widget cannot be installed</h3><p>The package did not pass pre-install validation.</p></div><button class="icon-button" data-close-install type="button"><span class="material-symbols-outlined">close</span></button></header><div class="install-error"><span class="material-symbols-outlined">warning</span><p>${escapeHtml(state.installError)}</p></div><footer class="modal-actions"><button class="secondary-action" data-close-install type="button">Close</button></footer></section>`;
    bindInstallModal();
    localizeIcons();
    return;
  }
  const preview = state.installPreview;
  const permissions = permissionEntries(preview.permissions);
  const operation = preview.isUpdate ? "Update" : "Install";
  const blockedExisting = preview.alreadyInstalled && !preview.isUpdate;
  root.innerHTML = `
    <div class="modal-backdrop" data-close-install></div>
    <section class="widget-modal install-modal glass-panel" role="dialog" aria-modal="true" aria-label="${operation} ${escapeAttr(preview.displayName)}">
      <header class="modal-head">
        <div class="widget-title-block"><div class="widget-icon"><span class="material-symbols-outlined">extension</span></div><div><h3>${operation} ${escapeHtml(preview.displayName)}</h3><p>${escapeHtml(preview.id)} · ${preview.isUpdate ? `v${escapeHtml(preview.installedVersion)} → ` : ""}v${escapeHtml(preview.version)}</p></div></div>
        <button class="icon-button" data-close-install type="button"><span class="material-symbols-outlined">close</span></button>
      </header>
      <div class="install-summary">
        <p>${escapeHtml(preview.description)}</p>
        <div class="install-meta"><span>Author</span><strong>${escapeHtml(preview.authorName)}</strong><span>Provider</span><strong>${escapeHtml(preview.providerType)}</strong></div>
      </div>
      <div class="permission-review">
        <div><span class="material-symbols-outlined">shield</span><div><h4>Requested permissions</h4><p>Review what this widget version can access before ${preview.isUpdate ? "updating" : "installing"}.</p></div></div>
        ${permissions.length ? `<ul>${permissions.map(([key, value]) => `<li><strong>${escapeHtml(permissionLabel(key, value))}</strong></li>`).join("")}</ul>` : `<p class="permission-none">This widget requests no additional permissions.</p>`}
      </div>
      <label class="install-enable"><input id="enable-installed-widget" type="checkbox" ${state.installEnable ? "checked" : ""}/><span>${preview.isUpdate ? "Keep this widget enabled after updating" : "Enable this widget after installation"}</span></label>
      ${blockedExisting ? `<div class="install-warning">Version ${escapeHtml(preview.installedVersion || "unknown")} is already installed. Updates must have a higher version number.</div>` : ""}
      <footer class="modal-actions">
        <button class="secondary-action" data-close-install type="button">Cancel</button>
        <button class="gradient-action" id="confirm-widget-install" type="button" ${blockedExisting ? "disabled" : ""}><span class="material-symbols-outlined">${preview.isUpdate ? "sync" : "download"}</span><span>Accept & ${operation}</span></button>
      </footer>
    </section>`;
  bindInstallModal();
  localizeIcons();
}

function closeInstallModal() {
  state.installPreview = null;
  state.installError = "";
  state.installSource = "";
  renderInstallModal();
}

function bindInstallModal() {
  document.querySelectorAll("[data-close-install]").forEach((button) => {
    button.onclick = () => closeInstallModal();
  });
  document.getElementById("enable-installed-widget")?.addEventListener("change", (event) => {
    state.installEnable = event.target.checked;
  });
  document.getElementById("confirm-widget-install")?.addEventListener("click", async (event) => {
    const button = event.currentTarget;
    button.disabled = true;
    const source = state.installSource;
    const preview = state.installPreview;
    const enableAfter = state.installEnable;
    try {
      const id = await invoke("install_community_widget", { source, approvedPermissions: true, replaceExisting: Boolean(preview?.isUpdate) });
      closeInstallModal();
      setStatus(`${id} ${preview?.isUpdate ? "updated" : "installed"}; validating…`);
      setTimeout(async () => {
        try {
          const loaded = await invoke("load_state");
          applyRuntimeCatalog(loaded.widgetCatalog);
          state.settings = mergeSettings(state.settings);
          const installed = widgetState(id);
          installed.settings._permissionsApproved = true;
          installed.enabled = enableAfter;
          state.settings.activeDesign = id;
          state.page = "library";
          setDirty(true);
          await saveSettings();
          render();
          setStatus(`${id} ${preview?.isUpdate ? `updated to ${preview.version}` : `installed${installed.enabled ? " and enabled" : ""}`}`);
        } catch (error) { setStatus(`Installed, but catalog refresh failed: ${error}`); }
      }, 1200);
    } catch (error) {
      state.installError = String(error);
      state.installPreview = null;
      renderInstallModal();
    }
  });
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
        ${catalog.supportsMultipleInstances ? `<button class="secondary-action" data-duplicate-widget="${catalog.id}" type="button"><span class="material-symbols-outlined">add</span><span>Duplicate</span></button>` : ""}
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
  const dynamic = widgetById(id);
  if (dynamic?.local) {
    const markup = dynamicPreviewNode(dynamic.layout, widgetState(id).settings || {});
    return {
      preview: `<div class="native-preview-stage"><div class="native-widget dynamic-widget" style="width:${Number(dynamic.width || 96)}px;height:${Number(dynamic.height || 24)}px">${markup}</div></div>`,
      taskbar: `<div class="dynamic-widget">${markup}</div>`,
    };
  }
  if (id.startsWith("system-")) return systemWidgetRuntime(id);
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

function dynamicPreviewNode(node, settings) {
  if (!node || typeof node !== "object") return "--";
  const type = node.type;
  if (type === "row" || type === "column") return `<span style="display:flex;flex-direction:${type === "row" ? "row" : "column"};gap:${clampNumber(node.gap, 0, 24, 0)}px;align-items:center">${(node.children || []).map((child) => dynamicPreviewNode(child, settings)).join("")}</span>`;
  if (type === "text") {
    const segments = Array.isArray(node.segments) ? node.segments : [node];
    const text = segments.map((segment) => segment.text || (String(segment.bind || "").startsWith("settings.") ? settings[String(segment.bind).slice(9)] ?? "--" : String(segment.bind || "").endsWith("time") ? new Date().toLocaleTimeString() : "--")).join("");
    return `<span style="font-size:${clampNumber(node.fontSize, 8, 18, 10)}px;color:${node.color === "systemAccent" ? "#2986cc" : escapeAttr(node.color || "#fff")}">${escapeHtml(text)}</span>`;
  }
  if (type === "icon") return `<span class="material-symbols-outlined" style="color:${node.color === "systemAccent" ? "#2986cc" : escapeAttr(node.color || "#fff")}">extension</span>`;
  if (type === "spacer") return `<span style="display:block;width:${clampNumber(node.width, 0, 96, 3)}px;height:${clampNumber(node.height, 0, 48, 1)}px"></span>`;
  if (type === "divider") return `<i style="display:block;width:1px;height:${clampNumber(node.height, 1, 48, 16)}px;background:#66fff"></i>`;
  if (type === "bar" || type === "progress" || type === "sparkline") return `<i class="quota-bar" style="width:${clampNumber(node.width, 8, 240, 32)}px"><em style="width:55%"></em></i>`;
  if (type === "pie") return `<i class="xmeter-pie" style="--a:55;--b:0"></i>`;
  return `<span>◇</span>`;
}

function systemWidgetRuntime(id) {
  const widget = widgetState(id);
  const values = widget.settings || {};
  const accent = "#2986cc";
  const resolve = (value, fallback) => String(value || fallback).toLowerCase() === "systemaccent" ? accent : value || fallback;
  const defaults = id === "system-cpu"
    ? { mode: "bar", first: resolve(values.userColor, accent), second: resolve(values.systemColor, "#fff"), outline: resolve(values.outlineColor, "#fff") }
    : id === "system-storage"
      ? { mode: "text", first: resolve(values.readColor, "#fff"), second: resolve(values.writeColor, "#fff"), outline: resolve(values.outlineColor, "#fff") }
      : id === "system-network"
        ? { mode: "text", first: resolve(values.sendColor, accent), second: resolve(values.receiveColor, accent), outline: resolve(values.outlineColor, "#fff") }
        : { mode: "pie", first: resolve(values.usedColor, accent), second: "transparent", outline: resolve(values.outlineColor, accent) };
  const mode = values.displayMode || defaults.mode;
  const coreValues = [[18, 4], [42, 7], [35, 6], [9, 3], [54, 8], [31, 5], [47, 7], [23, 4]];
  const perCore = id === "system-cpu" && values.showIndividualCores !== false;
  const cores = values.combineLogicalCores ? coreValues.reduce((rows, value, index) => {
    if (index % 2 === 0) rows.push([value[0], value[1]]); else { rows.at(-1)[0] = Math.round((rows.at(-1)[0] + value[0]) / 2); rows.at(-1)[1] = Math.round((rows.at(-1)[1] + value[1]) / 2); } return rows;
  }, []) : coreValues;
  const split = values.separateUtilization !== false;
  const cpuColor = resolve(values.cpuColor, accent);
  const style = `--primary:${escapeAttr(split ? defaults.first : cpuColor)};--secondary:${escapeAttr(defaults.second)};--meter-outline:${escapeAttr(defaults.outline)}`;
  let meter = "";
  if (mode === "text") {
    if (perCore) meter = `<span class="xmeter-core-text">${cores.map(([user, system]) => `<i><b>${split ? system : user + system}%</b><small>${split ? `${user}%` : ""}</small></i>`).join("")}</span>`;
    else if (id === "system-storage") meter = `<span class="xmeter-rate-lines"><span><b>24 MB/s</b><i>▲</i></span><span><small>8 MB/s</small><i>▼</i></span></span>`;
    else if (id === "system-network") meter = `<span class="xmeter-rate-lines"><span><b>2 MB/s</b><i>▲</i></span><span><small>12 MB/s</small><i>▼</i></span></span>`;
    else meter = `<span class="xmeter-single-text">${id === "system-memory" ? "64%" : "45%"}</span>`;
  } else if (mode === "bar") {
    const metrics = perCore ? cores : id === "system-storage" ? [[57, 18]] : id === "system-network" ? [[16, 42]] : id === "system-memory" ? [[64, 0]] : [[37, 8]];
    meter = `<span class="xmeter-bars">${metrics.map(([first, second]) => `<i class="xmeter-vbar" style="--a:${first};--b:${split || id !== "system-cpu" ? second : 0}"><em></em><b></b></i>`).join("")}</span>`;
  } else {
    const metrics = perCore ? cores : id === "system-storage" ? [[57, 18]] : id === "system-network" ? [[16, 42]] : id === "system-memory" ? [[64, 0]] : [[37, 8]];
    meter = `<span class="xmeter-pies">${metrics.map(([first, second]) => `<i class="xmeter-pie" style="--a:${first};--b:${split || id !== "system-cpu" ? second : 0}"></i>`).join("")}</span>`;
  }
  const markup = `<div class="native-widget system-meter ${id} ${mode}" style="${style}">${meter}</div>`;
  return { preview: `<div class="native-preview-stage">${markup}</div>`, taskbar: markup };
}

function statusRow(label, value) {
  return `<div class="status-row"><span>${escapeHtml(label)}</span><strong>${escapeHtml(value)}</strong></div>`;
}

function bindLibraryPage() {
  document.getElementById("widget-search")?.addEventListener("input", (event) => {
    state.search = event.target.value;
    render();
  });
  document.getElementById("check-community-updates")?.addEventListener("click", async () => {
    setStatus("Checking community widget updates…");
    await loadRemoteLibrary(true);
    if (state.remoteLibraryState === "error") {
      setStatus(`Update check failed: ${state.remoteLibraryError}`);
      return;
    }
    const count = widgetCatalog.filter((widget) => remoteUpdateFor(widget.id)).length;
    setStatus(count ? `${count} community widget update${count === 1 ? "" : "s"} available` : "Community widgets are up to date");
  });
  document.querySelectorAll("[data-update-community]").forEach((button) => {
    button.addEventListener("click", (event) => {
      event.preventDefault();
      event.stopPropagation();
      downloadAndReviewWidget(button.dataset.updateCommunity, button);
    });
  });
  document.querySelectorAll("[data-remove-installed]").forEach((button) => {
    button.addEventListener("click", (event) => {
      event.preventDefault();
      event.stopPropagation();
      removeCommunityWidget(button.dataset.removeInstalled, true);
    });
  });
  bindWidgetButtons();
}

function bindWidgetButtons() {
  document.querySelectorAll("[data-toggle-widget]").forEach((button) => {
    button.onclick = (event) => {
      event.stopPropagation();
      const id = button.dataset.toggleWidget;
      const widget = widgetState(id);
      const manifest = widgetById(id);
      if (!widget.enabled && manifest?.local) {
        const requested = Object.entries(manifest.permissions || {})
          .filter(([, value]) => Array.isArray(value) ? value.length : Boolean(value));
        if (requested.length) {
          const details = requested.map(([key, value]) => `${key}: ${Array.isArray(value) ? value.join(", ") : value}`).join("\n");
          if (!window.confirm(`This local/unverified widget requests:\n\n${details}\n\nEnable and approve these permissions?`)) return;
          widget.settings._permissionsApproved = true;
        }
      }
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
    element.onkeydown = (event) => {
      if (event.target !== element) return;
      if (event.key !== "Enter" && event.key !== " ") return;
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
  document.querySelector("[data-duplicate-widget]")?.addEventListener("click", (event) => {
    const design = event.currentTarget.dataset.duplicateWidget;
    const source = widgetState(design);
    const instanceId = `${design}-${Date.now().toString(36)}`;
    state.settings.widgets.push({
      ...source,
      id: instanceId,
      enabled: true,
      moveX: Math.max(-640, Number(source.moveX || 0) - 24),
      order: state.settings.widgets.length,
      settings: { ...(source.settings || {}) },
    });
    setDirty(true); scheduleAutosave(); setStatus(`Created ${instanceId}`); render();
  });
  bindInputs();
  bindSystemMeterTabs();
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
  bindSystemMeterTabs();
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
  document.querySelectorAll("[data-color-target]").forEach((input) => {
    input.addEventListener("input", () => {
      const textInput = document.querySelector(`[data-instance-setting="${input.dataset.colorTarget}"]`);
      if (!textInput) return;
      textInput.value = input.value.toUpperCase();
      textInput.dispatchEvent(new Event("input", { bubbles: true }));
    });
  });
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
      if (key === "positionPct" || key === "moveX") {
        locallyEditedWidgetPositions.add(widget.design);
      }
      state.settings.enabled = widget.enabled;
      state.settings.widgetMoveX = widget.moveX;
      state.settings.widgetOffsetPx = Math.max(0, -widget.moveX);
      setDirty(true);
      scheduleAutosave();
      renderFloatingTaskbar();
    });
  });
  document.querySelectorAll("[data-instance-setting]").forEach((input) => {
    input.addEventListener("input", () => {
      const widget = activeWidget();
      widget.settings ||= {};
      const key = input.dataset.instanceSetting;
      widget.settings[key] = input.type === "checkbox"
        ? input.checked
        : input.type === "number" || input.type === "range"
          ? clampNumber(input.value, Number(input.min || 0), Number(input.max || 1000000000), Number(widget.settings[key] ?? 0))
          : input.value;
      if (input.type === "range") updateValueLabel("instance", key, Number(widget.settings[key]).toFixed(1), input.dataset.unit || "");
      setDirty(true);
      scheduleAutosave();
      renderFloatingTaskbar();
      if (["displayMode", "showIndividualCores", "separateUtilization", "autoBandwidth"].includes(key)) {
        if (state.modalWidgetId) renderWidgetModal(); else renderPage();
      }
    });
  });
}

function bindSystemMeterTabs() {
  document.querySelectorAll("[data-system-meter-tab]").forEach((tab) => {
    tab.addEventListener("click", (event) => {
      if (event.target.matches("[data-system-tab-enabled]")) return;
      state.settings.activeDesign = tab.dataset.systemMeterTab;
      if (state.modalWidgetId) state.modalWidgetId = tab.dataset.systemMeterTab;
      render();
    });
    tab.addEventListener("dragstart", (event) => event.dataTransfer.setData("text/plain", tab.dataset.systemMeterTab));
    tab.addEventListener("dragover", (event) => event.preventDefault());
    tab.addEventListener("drop", (event) => {
      event.preventDefault();
      reorderSystemMeters(event.dataTransfer.getData("text/plain"), tab.dataset.systemMeterTab);
    });
  });
  document.querySelectorAll("[data-system-tab-enabled]").forEach((checkbox) => {
    checkbox.addEventListener("click", (event) => event.stopPropagation());
    checkbox.addEventListener("change", () => {
      widgetState(checkbox.dataset.systemTabEnabled).enabled = checkbox.checked;
      setDirty(true); scheduleAutosave(); renderFloatingTaskbar();
    });
  });
}

function reorderSystemMeters(fromId, toId) {
  if (!fromId || !toId || fromId === toId) return;
  const all = [...state.settings.widgets].sort((a, b) => a.order - b.order);
  const slots = all.map((widget, index) => widget.design.startsWith("system-") ? index : -1).filter((index) => index >= 0);
  const systems = slots.map((index) => all[index]);
  const from = systems.findIndex((widget) => widget.design === fromId);
  const to = systems.findIndex((widget) => widget.design === toId);
  if (from < 0 || to < 0) return;
  const [moved] = systems.splice(from, 1); systems.splice(to, 0, moved);
  slots.forEach((slot, index) => { all[slot] = systems[index]; });
  state.settings.widgets = all.map((widget, index) => ({ ...widget, order: index }));
  setDirty(true); scheduleAutosave(); render();
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
    locallyEditedWidgetPositions.clear();
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
    state.systemSources = loaded.systemSources || state.systemSources;
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
