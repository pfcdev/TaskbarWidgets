const { invoke } = window.__TAURI__.core;

const supportLinks = {
  github: "https://github.com/pfcdev/TaskbarWidgets",
  reddit: "https://www.reddit.com/r/windowsapps/comments/1v5n3qs/i_built_an_opensource_windows_11_app_that_adds/",
  x: "https://x.com/pfcdev",
  feedback: "https://github.com/pfcdev/TaskbarWidgets/issues/new?labels=feedback&title=%5BFeedback%5D%20",
};

const widgetThumbnailTemplate = "./assets/widgets/widget-thumbnail-template.svg";

const widgetPresentation = {
  "codex-status": { icon: "terminal", accent: "#5fd4ff", featured: true },
  "weather-static": { icon: "partly_cloudy_day", accent: "#f59e0b", featured: true },
  "discord-voice": { icon: "forum", accent: "#5865f2" },
  "parking-lot": { icon: "inventory_2", accent: "#94a3b8", featured: true },
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
  discordRealTimeVoiceEnabled: false,
  mediaDarkMode: true,
  mediaShowControls: true,
  mediaControlsPosition: "right",
  mediaShowVisualizer: true,
  mediaVisualizerPosition: "right",
  mediaVisualizerBarCount: 10,
  mediaVisualizerCentered: false,
  mediaVisualizerBaseline: false,
  mediaVisualizerBaselineAutoHide: false,
  mediaVisualizerSensitivity: 2,
  mediaVisualizerPeakLevel: 3,
  mediaShowPauseOverlay: true,
  mediaHideWhenInactive: false,
  mediaAutoHidePaused: false,
  mediaScrollingEnabled: true,
  mediaScrollingSpeed: 18,
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
    description: "Configure the active widget queue, order, transitions, and timing.",
  },
  updates: {
    title: "System Updates",
    description: "Manage the Stable release channel, check GitHub releases, and install updates.",
  },
  settings: {
    title: "Settings",
    description: "Manage TaskbarWidgets behavior, widget settings, and system integrations.",
  },
  help: {
    title: "Help & Resources",
    description: "Find documentation, community discussions, and project support.",
  },
};

let state = {
  appDir: "",
  page: "library",
  settings: { ...defaults },
  updateStatus: {},
  mediaStatus: {},
  webRenderHealth: {},
  voiceHelperInstalled: false,
  voiceHelperBusy: false,
  systemSources: { disks: [], interfaces: [] },
  runtimeCatalog: { widgets: [] },
  communityWidgetsDir: "",
  releaseTimeline: [],
  releaseTimelineState: "idle",
  search: "",
  dirty: false,
  status: "",
  modalWidgetId: "",
  installPreview: null,
  installSource: "",
  installError: "",
  installEnable: true,
  installAdvertisedPermissions: null,
  installOptionalGrants: [],
  remoteLibrary: [],
  remoteLibraryState: "idle",
  remoteLibraryError: "",
  remoteSearch: "",
  communityLastCheckedAt: 0,
  communityUpdates: [],
};

let autosaveTimer = 0;
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
  for (const manifest of state.runtimeCatalog.widgets.filter((item) =>
    item.valid && (item.renderer === "declarative" || item.renderer === "native" || item.renderer === "web"))) {
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

function availableCommunityPosition(widget) {
  if (widget.positionPct !== 100 || widget.moveX !== 0) return widget.positionPct;
  const occupied = state.settings.widgets
    .filter((item) => item.enabled && item.id !== widget.id)
    .map((item) => Number(item.positionPct ?? 100));
  const candidates = [75, 50, 25, 0, 100];
  return candidates.find((candidate) =>
    occupied.every((position) => Math.abs(position - candidate) >= 18)) ?? 50;
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
    state.webRenderHealth = loaded.webRenderHealth || {};
    state.voiceHelperInstalled = Boolean(loaded.voiceHelperInstalled);
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
    state.voiceHelperInstalled = Boolean(loaded.voiceHelperInstalled);
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
  renderWidgetModal();
  renderInstallModal();
  localizeIcons();
}

function localizeIcons() {
  const glyphs = {
    widgets: "\uE71D", terminal: "\uE756", partly_cloudy_day: "\uE706", forum: "\uE8BD",
    play_circle: "\uE768", download: "\uE896", rebase_edit: "\uE8AB", system_update: "\uE895",
    settings: "\uE713", help: "\uE897", chat_bubble: "\uE8F2", search: "\uE721",
    pending: "\uE823", check_circle: "\uE73E", check: "\uE73E", add: "\uE710", view_timeline: "\uE8A5",
    desktop_windows: "\uE7F4", drag_indicator: "\uE700", keyboard_arrow_up: "\uE70E",
    keyboard_arrow_down: "\uE70D", close: "\uE711", tune: "\uE9E9", extension: "\uE74C",
    warning: "\uE7BA", new_releases: "\uEA8F", hourglass_top: "\uE916", sync: "\uE895",
    system_update_alt: "\uE896", history: "\uE81C", save: "\uE74E", folder_open: "\uE838",
    grid_view: "\uE80A", folder: "\uE8B7", language: "\uE774", expand_less: "\uE70E", wifi: "\uE701",
    volume_up: "\uE767", window: "\uE737", eject: "\uF847", play_arrow: "\uE768", light_mode: "\uE706",
    shield: "\uEA18", delete: "\uE74D", memory: "\uE950", hard_drive: "\uEDA2", swap_vert: "\uE8D4",
    developer_board: "\uE950", play_circle_filled: "\uE768", refresh: "\uE72C", open_in_new: "\uE8A7"
  };
  document.querySelectorAll(".material-symbols-outlined:not([data-localized-icon])").forEach((element) => {
    const name = element.textContent.trim();
    element.textContent = glyphs[name] || "\uE74C";
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
  document.querySelectorAll("[data-external-link]").forEach((button) => {
    button.onclick = () => openExternalUrl(supportLinks[button.dataset.externalLink]);
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
  } else if (state.page === "help") {
    page.innerHTML = helpPage();
    bindExternalLinks();
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
      ? `<section class="fluent-card channel-card library-unavailable"><span class="status-chip">Library offline</span><h3>The remote library is not ready yet</h3><p>${escapeHtml(state.remoteLibraryError || "The server did not return a valid index.json.")}</p><button class="secondary-action" id="retry-remote-library" type="button"><span class="material-symbols-outlined">sync</span><span>Try Again</span></button></section>`
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

const permissionCatalog = {
  "accounts.list.read": ["high", "Kayıtlı hesapları görebilir", "Taskbar Widgets içinde kayıtlı hesapların listesini okuyabilir."],
  "accounts.profile.read": ["high", "Hesap profillerini okuyabilir", "Kayıtlı hesapların profil ve kimlik bilgilerini okuyabilir."],
  "accounts.history.read": ["critical", "Tüm hesap geçmişini okuyabilir", "Taskbar Widgets içindeki hesapların geçmiş kullanım bilgilerine erişebilir."],
  "accounts.tokens.read": ["critical", "Hesap oturum bilgilerini okuyabilir", "Kayıtlı token ve oturum verilerine erişebilir."],
  "accounts.active.write": ["high", "Aktif hesabı değiştirebilir", "Taskbar Widgets tarafından kullanılan aktif hesabı değiştirebilir."],
  "accounts.delete": ["critical", "Hesapları silebilir", "Taskbar Widgets içinde kayıtlı hesapları silebilir."],
  "filesystem.read": ["high", "Dosyaları okuyabilir", "Belirtilen dosya ve klasörlerin içeriğini okuyabilir."],
  "filesystem.write": ["high", "Dosyaları değiştirebilir", "Belirtilen dosya ve klasörlerde veri oluşturabilir veya değiştirebilir."],
  "filesystem.delete": ["critical", "Dosyaları silebilir", "Belirtilen dosya ve klasörlerdeki verileri silebilir."],
  "filesystem.watch": ["medium", "Dosya değişikliklerini izleyebilir", "Belirtilen klasörlerdeki değişiklikleri arka planda takip edebilir."],
  "filesystem.all": ["critical", "Tüm kullanıcı dosyalarına erişebilir", "Windows hesabınızın erişebildiği bütün dosyaları okuyabilir, değiştirebilir veya silebilir."],
  "registry.read": ["high", "Registry verilerini okuyabilir", "Belirtilen Windows Registry anahtarlarını okuyabilir."],
  "registry.write": ["high", "Registry verilerini değiştirebilir", "Belirtilen Windows Registry anahtarlarını oluşturabilir veya değiştirebilir."],
  "registry.delete": ["critical", "Registry verilerini silebilir", "Belirtilen Windows Registry anahtarlarını silebilir."],
  "registry.all": ["critical", "Registry üzerinde tam erişim kullanabilir", "Windows hesabınızın erişebildiği tüm Registry alanlarını okuyabilir ve değiştirebilir."],
  "process.list": ["medium", "Çalışan uygulamaları görebilir", "Bilgisayarda çalışan process listesini ve temel bilgilerini okuyabilir."],
  "process.start": ["high", "Program çalıştırabilir", "Bilgisayarınızda executable veya script başlatabilir."],
  "process.stop": ["high", "Programları durdurabilir", "Çalışan process'leri sonlandırabilir."],
  "process.control": ["critical", "Diğer programları kontrol edebilir", "Çalışan uygulamalara komut gönderebilir ve durumlarını değiştirebilir."],
  "process.inject": ["critical", "Diğer process'lere kod yükleyebilir", "Çalışan uygulamaların belleğine kod yükleyebilir."],
  "shell.execute": ["critical", "Komut çalıştırabilir", "PowerShell, Komut İstemi veya başka shell komutları yürütebilir."],
  "shell.openExternal": ["medium", "Bağlantı ve dosya açabilir", "Varsayılan Windows uygulamalarıyla bağlantı veya dosya açabilir."],
  "network.internet": ["medium", "İnternete bağlanabilir", "Belirtilen internet adresleriyle veri alışverişi yapabilir."],
  "network.local": ["high", "Yerel ağa erişebilir", "Aynı ağdaki bilgisayar ve cihazlarla iletişim kurabilir."],
  "network.listen": ["high", "Ağ bağlantısı kabul edebilir", "Bilgisayarınızda bir ağ portu açıp gelen bağlantıları kabul edebilir."],
  "network.unrestricted": ["critical", "Sınırsız ağ erişimi kullanabilir", "İnternet ve yerel ağdaki tüm adreslerle sınırsız iletişim kurabilir."],
  "windows.win32": ["high", "Win32 API kullanabilir", "Windows masaüstü ve sistem API'lerine erişebilir."],
  "windows.winrt": ["high", "Windows Runtime API kullanabilir", "Windows Runtime özelliklerine erişebilir."],
  "windows.com": ["high", "COM bileşenlerini kullanabilir", "Windows ve kurulu uygulamaların COM arayüzlerine erişebilir."],
  "windows.wmi": ["high", "WMI sistem bilgilerine erişebilir", "WMI üzerinden sistem bilgisi okuyabilir veya işlem başlatabilir."],
  "clipboard.read": ["high", "Panoyu okuyabilir", "Kopyaladığınız metin ve diğer pano verilerini okuyabilir."],
  "clipboard.write": ["medium", "Panoyu değiştirebilir", "Windows panosunun içeriğini değiştirebilir."],
  "notifications.show": ["low", "Bildirim gösterebilir", "Windows bildirim merkezinde bildirim oluşturabilir."],
  camera: ["critical", "Kamerayı kullanabilir", "Bağlı kameradan görüntü alabilir."],
  microphone: ["critical", "Mikrofonu kullanabilir", "Bağlı mikrofondan ses alabilir."],
  location: ["high", "Konumunuzu okuyabilir", "Windows konum servisinden konum bilgisi alabilir."],
  bluetooth: ["high", "Bluetooth cihazlarına erişebilir", "Yakındaki veya eşleştirilmiş Bluetooth cihazlarıyla iletişim kurabilir."],
  usb: ["high", "USB cihazlarına erişebilir", "Bağlı USB cihazlarıyla iletişim kurabilir."],
  "media.sessions.read": ["medium", "Çalan medyayı görebilir", "Windows medya oturumlarındaki parça, sanatçı ve oynatma durumunu okuyabilir."],
  "media.playback.control": ["high", "Medya oynatmayı kontrol edebilir", "Oynat, duraklat, ileri ve geri komutları gönderebilir."],
  "steam.downloads.read": ["high", "Steam indirmelerini okuyabilir", "Steam kütüphaneleri, manifestleri ve indirme durumunu okuyabilir."],
  "steam.client.control": ["high", "Steam istemcisini kontrol edebilir", "Steam istemcisine komut gönderebilir veya Steam bağlantıları açabilir."],
  "discord.state.read": ["high", "Discord durumunu okuyabilir", "Discord kullanıcı, kanal ve ses durumu bilgilerine erişebilir."],
  "system.metrics.read": ["low", "Sistem performansını okuyabilir", "CPU, bellek, disk ve ağ kullanım metriklerini okuyabilir."],
  "taskbar.control": ["high", "Görev çubuğunu kontrol edebilir", "Taskbar Widgets görev çubuğu yüzeyini ve yerleşimini değiştirebilir."],
  "settings.read": ["medium", "Uygulama ayarlarını okuyabilir", "Taskbar Widgets ayarlarını okuyabilir."],
  "settings.write": ["high", "Uygulama ayarlarını değiştirebilir", "Taskbar Widgets ayarlarını değiştirebilir."],
  "system.fullAccess": ["critical", "Windows hesabınızda tam erişim kullanabilir", "Windows kullanıcı hesabınızın erişebildiği dosyalara, uygulamalara, hesaplara, Registry verilerine ve ağ kaynaklarına erişebilir."],
  "system.administrator": ["critical", "Yönetici yetkisi isteyebilir", "Windows UAC onayıyla yönetici olarak çalışabilir ve sistem genelinde değişiklik yapabilir."],
  "system.startup": ["high", "Windows başlangıcında çalışabilir", "Windows oturumu açıldığında otomatik olarak başlayabilir."],
  "system.background": ["medium", "Arka planda çalışabilir", "Widget görünür değilken de arka planda çalışmaya devam edebilir."],
  "legacy.network": ["medium", "İnternete bağlanabilir", "Listelenen HTTPS adreslerinden JSON verisi alabilir."],
  "legacy.systemMetrics": ["low", "Sistem performansını okuyabilir", "İzin verilen CPU, bellek, disk veya ağ metriklerini okuyabilir."],
  "legacy.openExternal": ["medium", "Bağlantı açabilir", "Listelenen internet adreslerini varsayılan tarayıcıda açabilir."],
  "legacy.storage": ["low", "Özel widget depolaması kullanabilir", "Yalnızca kendi widget verilerini yerel olarak saklayabilir."],
  "legacy.graphics": ["low", "Gelişmiş grafik kullanabilir", "WebGL veya sürekli animasyon çalıştırabilir."]
};

function permissionEntries(permissions) {
  if (Array.isArray(permissions?.required)) {
    return [
      ...permissions.required.map((request) => ({ ...request, optional: false })),
      ...(Array.isArray(permissions.optional) ? permissions.optional.map((request) => ({ ...request, optional: true })) : []),
    ];
  }
  return Object.entries(permissions || {})
    .filter(([, value]) => Array.isArray(value) ? value.length : Boolean(value))
    .map(([key, value]) => ({
      id: `legacy.${key}`,
      scope: Array.isArray(value) ? value : undefined,
      reason: "",
      optional: false,
    }));
}

function permissionInfo(request) {
  const [risk, title, description] = permissionCatalog[request.id] || ["high", request.id, "Bu widget bu sistem yetkisini kullanmak istiyor."];
  const scope = Array.isArray(request.scope) ? request.scope.join(", ") : request.scope;
  return { risk, title, description, scope: scope ? String(scope) : "" };
}

function permissionLabel(request) {
  const info = permissionInfo(request);
  return `${info.title}${info.scope ? `: ${info.scope}` : ""}${request.optional ? " (isteğe bağlı)" : ""}`;
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
  return `<article class="fluent-card remote-widget-card" style="--accent:#5fd4ff">
    ${widgetThumbnail("Explore widget artwork")}
    <div class="remote-widget-body">
      <div class="remote-widget-head"><span class="widget-icon"><span class="material-symbols-outlined">extension</span></span><span class="status-chip">${escapeHtml(widget.category || "Community")}</span><span class="status-chip">${widget.renderer === "web" ? "Legacy Web" : widget.renderer === "native" ? "Native XAML" : "Native DSL"}</span></div>
      <h3>${escapeHtml(widget.displayName)}</h3>
      <p>${escapeHtml(widget.description)}</p>
      <div class="remote-author">By <strong>${escapeHtml(widget.author?.name || "Unknown author")}</strong> · v${escapeHtml(widget.version)}</div>
      <div class="permission-chips">${permissions.length ? permissions.map((request) => `<span class="permission-chip-${escapeAttr(permissionInfo(request).risk)}">${escapeHtml(permissionLabel(request))}</span>`).join("") : "<span>No additional permissions</span>"}</div>
      ${updateAvailable ? `<div class="update-version-line"><strong>Update available</strong><span>v${escapeHtml(installedWidget.version)} → v${escapeHtml(widget.version)}</span></div>` : ""}
      <button class="${installed && !updateAvailable ? "secondary-action" : "accent-action"}" data-download-remote="${escapeAttr(widget.id)}" type="button" ${installed && !updateAvailable ? "disabled" : ""}>
        <span class="material-symbols-outlined">${updateAvailable ? "sync" : installed ? "check_circle" : "download"}</span><span>${updateAvailable ? "Review & Update" : installed ? "Installed" : "Review & Install"}</span>
      </button>
    </div>
  </article>`;
}

function widgetThumbnail(label = "Widget artwork") {
  return `<div class="widget-thumbnail"><img src="${widgetThumbnailTemplate}" alt="${escapeAttr(label)}" /><span>Template</span></div>`;
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
    const download = await invoke("download_remote_widget", { widgetId });
    setStatus("Package verified");
    await openWidgetInstall(download.source, download.advertisedPermissions);
  } catch (error) {
    setStatus(`Download failed: ${error}`);
    if (button) button.disabled = false;
  }
}

async function removeCommunityWidget(widgetId, ask = true) {
  const manifest = widgetCatalog.find((item) => item.id === widgetId) ||
    (state.runtimeCatalog.widgets || []).find((item) => item.id === widgetId && !item.trusted);
  if (!manifest || manifest.trusted ||
      (!manifest.local && manifest.renderer !== "declarative" && manifest.renderer !== "native" && manifest.renderer !== "web")) return;
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
  const renderHealth = state.webRenderHealth || {};
  return `${pageHeader("developer")}
    <div class="developer-grid">
    <section class="fluent-card channel-card developer-card developer-folder-card">
      <div class="developer-card-head"><span class="widget-icon"><span class="material-symbols-outlined">folder_open</span></span><div><h3>Community widget folder</h3><p>Install and manage local development packages.</p></div></div>
      <code class="developer-path">${escapeHtml(state.communityWidgetsDir || "%LocalAppData%\\TaskbarWidgets\\CommunityWidgets")}</code>
      <div class="developer-actions">
        <button class="accent-action" id="open-community-folder" type="button"><span class="material-symbols-outlined">folder_open</span><span>Open Folder</span></button>
        <button class="secondary-action" id="install-community-folder" type="button"><span class="material-symbols-outlined">add</span><span>Install Folder</span></button>
        <button class="secondary-action" id="install-community-package" type="button"><span class="material-symbols-outlined">download</span><span>Install .twidget</span></button>
        <button class="secondary-action" id="reload-community-catalog" type="button"><span class="material-symbols-outlined">sync</span><span>Reload</span></button>
      </div>
    </section>
    <section class="fluent-card channel-card developer-card">
      <div class="developer-card-head"><span class="widget-icon"><span class="material-symbols-outlined">developer_board</span></span><div><h3>Web renderer diagnostics</h3><p>Runtime state for approved web widgets.</p></div></div>
      <div class="diagnostics-card">
        ${statusRow("Active installation", state.appDir || "Unavailable")}
        ${statusRow("RenderHost", renderHealth.status || "stopped")}
        ${statusRow("Isolation", "Separate process · WebView2 Composition")}
        ${renderHealth.error ? statusRow("Last error", renderHealth.error) : ""}
      </div>
      <p class="developer-note">RenderHost and Edge processes stay stopped until an approved web widget is enabled.</p>
    </section>
    </div>
    <section class="widget-library-list" aria-label="Community validation results">
      ${community.length ? community.map((item) => `<article class="widget-library-row ${item.valid ? "enabled" : ""}">
        <div class="widget-icon"><span class="material-symbols-outlined">${item.valid ? "check_circle" : "warning"}</span></div>
        <div class="widget-library-copy"><div class="widget-library-title"><h3>${escapeHtml(item.displayName || item.id)}</h3><span>${item.valid ? "Local / Unverified" : "Rejected"}</span></div>
        <p>${escapeHtml(item.error || (item.valid ? `${item.id} · ${item.version}` : "Validation failed"))}</p></div>
        <button class="icon-button" data-remove-community="${escapeAttr(item.id)}" type="button" title="Remove local widget"><span class="material-symbols-outlined">close</span></button>
      </article>`).join("") : `<div class="library-empty"><strong>No local widgets</strong><span>Copy a widget folder here or run twdev dev.</span></div>`}
    </section>
    <section class="fluent-card channel-card developer-card developer-cli-card">
      <div class="developer-card-head"><span class="widget-icon"><span class="material-symbols-outlined">terminal</span></span><div><h3>Developer CLI</h3><p>Common commands for creating and validating a widget.</p></div></div>
      <div class="developer-command-list"><code>twdev init com.example.clock</code><code>twdev validate ./com.example.clock</code><code>twdev dev ./com.example.clock</code><code>twdev pack ./com.example.clock</code></div>
    </section>
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
      state.webRenderHealth = loaded.webRenderHealth || {};
      setStatus("Catalog reloaded"); render();
    } catch (error) { setStatus(`Reload failed: ${error}`); }
  });
}

function helpPage() {
  return `${pageHeader("help")}
    <section class="fluent-card help-intro">
      <span class="help-mark"><span class="material-symbols-outlined">help</span></span>
      <div><h3>How can we help?</h3><p>Start with the project documentation, ask the community, or send feedback directly through GitHub Issues.</p></div>
    </section>
    <section class="help-link-grid" aria-label="Project links">
      ${helpLinkCard("github", "terminal", "GitHub", "Source code, releases, documentation, and issue tracking.")}
      ${helpLinkCard("reddit", "forum", "Reddit", "Join the Taskbar Widgets discussion and share widget ideas.")}
      ${helpLinkCard("x", "chat_bubble", "X", "Follow project announcements and short development updates.")}
    </section>
    <section class="fluent-card help-steps">
      <h3>Quick help</h3>
      <div class="help-step"><span>1</span><div><strong>A widget is not visible</strong><p>Open Installed, turn the widget on, then check Explorer Integration under Settings.</p></div></div>
      <div class="help-step"><span>2</span><div><strong>A community widget will not install</strong><p>Open Developer and review its validation result and requested permissions.</p></div></div>
      <div class="help-step"><span>3</span><div><strong>Something is not working</strong><p>Use Feedback in the sidebar to open a pre-filled GitHub issue.</p></div></div>
    </section>
    ${inlineStatus()}`;
}

function helpLinkCard(key, icon, title, description) {
  return `<button class="fluent-card help-link-card" data-external-url="${escapeAttr(supportLinks[key])}" type="button">
    <span class="widget-icon"><span class="material-symbols-outlined">${icon}</span></span>
    <span><strong>${escapeHtml(title)}</strong><small>${escapeHtml(description)}</small></span>
    <span class="material-symbols-outlined">open_in_new</span>
  </button>`;
}

function bindExternalLinks() {
  document.querySelectorAll("[data-external-url]").forEach((button) => {
    button.addEventListener("click", () => openExternalUrl(button.dataset.externalUrl));
  });
}

async function openExternalUrl(url) {
  if (!url) return;
  try {
    await window.__TAURI__.shell.open(url);
  } catch (error) {
    setStatus(`Link could not be opened: ${error}`);
  }
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
  const enabled = isWidgetEnabled(widget.id);
  const update = remoteUpdateFor(widget.id);
  return `
    <article class="widget-library-row ${enabled ? "enabled" : ""}" style="--accent:${widget.accent}" data-open-widget="${widget.id}" role="button" tabindex="0" aria-label="Open ${escapeAttr(widget.title)} settings">
      ${widgetThumbnail(`${widget.title} artwork`)}
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
        ${widget.local ? `<button class="native-icon-action danger-action" data-remove-installed="${escapeAttr(widget.id)}" type="button" title="Remove widget" aria-label="Remove ${escapeAttr(widget.title)}"><span class="material-symbols-outlined">delete</span></button>` : ""}
        ${toggleButton(widget.id)}
      </div>
    </article>
  `;
}

function toggleButton(id) {
  const enabled = isWidgetEnabled(id);
  return `<label class="widget-toggle-control" title="${enabled ? "Disable" : "Enable"} ${escapeAttr(widgetById(id).title)}">
    <span>${enabled ? "On" : "Off"}</span>
    <span class="win-toggle"><input type="checkbox" data-toggle-widget="${escapeAttr(id)}" ${enabled ? "checked" : ""} aria-label="${enabled ? "Disable" : "Enable"} ${escapeAttr(widgetById(id).title)}" /><span><i></i></span></span>
  </label>`;
}

function rotationPage() {
  const queue = state.settings.rotationDesigns;
  return `
    ${pageHeader("rotation")}
    <section class="rotation-header">
      <div>
        <h3>Active Sequence</h3>
        <p>Choose widgets and arrange the order used by Slider Rotation.</p>
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
      <aside class="rotation-options fluent-card">
        <div class="rotation-option-copy"><span class="material-symbols-outlined">history</span><div><strong>Slide Interval</strong><p>Time before moving to the next widget.</p></div></div>
        <div class="rotation-interval-input"><input type="number" min="5" max="3600" data-setting="rotationIntervalSecs" value="${escapeAttr(state.settings.rotationIntervalSecs)}" /><span>sec</span></div>
        <div class="rotation-summary"><span>${queue.length}</span><small>widgets in sequence</small></div>
      </aside>
    </div>
    ${inlineStatus()}
  `;
}

function sequenceItem(id, index) {
  const widget = widgetById(id);
  return `
    <article class="sequence-item" draggable="true" data-sequence-id="${widget.id}" style="--accent:${widget.accent}">
      <div class="drag-handle"><span class="material-symbols-outlined">drag_indicator</span></div>
      <div class="sequence-icon"><span class="material-symbols-outlined">${widget.icon}</span></div>
      <div class="sequence-copy">
        <strong>${escapeHtml(widget.title)}</strong>
        <small>Queue #${index + 1}</small>
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
  const current = widgetById(state.settings.activeDesign);
  return `
    ${pageHeader("settings")}
    <div class="settings-stack">
      <section class="fluent-card settings-section">
        <div class="settings-section-head"><div class="section-title"><span class="material-symbols-outlined">tune</span><div><h3>Current Widget Settings</h3><p>Choose a widget, then adjust only its available options.</p></div></div></div>
        <div class="current-widget-toolbar">
          <div class="current-widget-identity" style="--accent:${current.accent}">
            <span class="widget-icon"><span class="material-symbols-outlined">${current.icon}</span></span>
            <div><strong>${escapeHtml(current.title)}</strong><small>${escapeHtml(current.category)} · ${isWidgetEnabled(current.id) ? "Enabled" : "Disabled"}</small></div>
          </div>
          <label class="widget-select-field"><span>Widget</span><select id="active-widget-select">
            ${widgetCatalog.map((widget) => `<option value="${escapeAttr(widget.id)}" ${state.settings.activeDesign === widget.id ? "selected" : ""}>${escapeHtml(widget.title)}</option>`).join("")}
          </select></label>
        </div>
        <div class="current-widget-fields">
          ${current.id.startsWith("system-") ? systemMeterTabs() : ""}
          ${currentWidgetSettingsFields()}
        </div>
      </section>

      <section class="fluent-card settings-section">
        <div class="settings-section-head"><div class="section-title"><span class="material-symbols-outlined">extension</span><div><h3>Explorer Integration</h3><p>Control the taskbar runtime without restarting Windows.</p></div></div></div>
        ${runtimeControlPanel()}
      </section>

      <section class="fluent-card settings-section danger-section">
        <div class="settings-section-head"><div class="section-title"><span class="material-symbols-outlined">warning</span><div><h3>Danger Zone</h3><p>Restore the last settings saved on this computer.</p></div></div></div>
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
    const discordWidget = activeWidget();
    const discordDisplayMode = discordWidget.settings?.displayMode || "avatars";
    const helperStatus = state.voiceHelperInstalled ? "Installed" : "Optional";
    const helperAction = state.voiceHelperInstalled
      ? `<button class="outline-danger" data-remove-voice-helper type="button" ${state.voiceHelperBusy ? "disabled" : ""}><span class="material-symbols-outlined">delete</span><span>Remove helper</span></button>`
      : `<button class="accent-action" data-install-voice-helper type="button" ${state.voiceHelperBusy ? "disabled" : ""}><span class="material-symbols-outlined">bolt</span><span>Enable instant detection</span></button>`;
    return `
      ${settingToggle("discordEnabled", "Discord Detection", state.settings.discordEnabled)}
      ${settingToggle("discordBackgroundEnabled", "Widget Background", state.settings.discordBackgroundEnabled)}
      ${instanceRadioSetting("displayMode", "Theme", discordDisplayMode, [["avatars", "Avatars"], ["channel", "Voice room"]])}
      <div class="setting-row">
        <div><strong>Local Discord Detection</strong><p>Reads the active voice channel and participant rows from the normal Discord window. No Discord application, OAuth login, or client secret is required.</p></div>
        <span class="status-chip">Automatic</span>
      </div>
      <div class="voice-helper-card">
        <div class="voice-helper-head">
          <div><strong>Instant Speaking Detection</strong><p>Makes the green speaking ring react immediately instead of waiting for Discord's periodic status update.</p></div>
          <span class="status-chip">${helperStatus}</span>
        </div>
        ${state.voiceHelperInstalled
          ? settingToggle("discordRealTimeVoiceEnabled", "Use instant detection", state.settings.discordRealTimeVoiceEnabled)
          : ""}
        <div class="discord-trust-note">
          <span class="material-symbols-outlined">verified_user</span>
          <p><strong>Discord stays untouched.</strong> Taskbar Widgets never changes Discord, adds anything to it, or listens to or records your calls. The optional Windows helper only notices when someone starts or stops speaking so the green ring can react instantly.</p>
        </div>
        <p class="voice-helper-consent">Windows asks for permission once when this helper is installed or removed. It runs only when instant detection is enabled.</p>
        ${state.voiceHelperBusy ? `<p class="voice-helper-feedback">Waiting for Windows approval…</p>` : ""}
        <div class="voice-helper-actions">${helperAction}</div>
      </div>
    `;
  }
  if (id === "media-player") {
    return `
      ${settingToggle("mediaDarkMode", "Dark Mode", state.settings.mediaDarkMode)}
      ${settingToggle("mediaShowControls", "Media Controls", state.settings.mediaShowControls)}
      ${selectSetting("mediaControlsPosition", "Controls Position", "Place previous, play or pause, and next before or after the media information.", state.settings.mediaControlsPosition, [["left", "Left"], ["right", "Right"]])}
      ${settingToggle("mediaShowVisualizer", "Real-time Audio Visualizer", state.settings.mediaShowVisualizer)}
      ${selectSetting("mediaVisualizerPosition", "Visualizer Position", "Place the visualizer before or after the media widget.", state.settings.mediaVisualizerPosition, [["left", "Left"], ["right", "Right"]])}
      ${rangeSetting("mediaVisualizerBarCount", "Visualizer Bars", "Number of frequency bars.", state.settings.mediaVisualizerBarCount, 1, 20)}
      ${settingToggle("mediaVisualizerCentered", "Centered Bars", state.settings.mediaVisualizerCentered)}
      ${settingToggle("mediaVisualizerBaseline", "Visualizer Baseline", state.settings.mediaVisualizerBaseline)}
      ${settingToggle("mediaVisualizerBaselineAutoHide", "Hide Baseline During Silence", state.settings.mediaVisualizerBaselineAutoHide)}
      ${rangeSetting("mediaVisualizerSensitivity", "Audio Sensitivity", "Raise quiet audio into view.", state.settings.mediaVisualizerSensitivity, 1, 3)}
      ${rangeSetting("mediaVisualizerPeakLevel", "Peak Level", "Calibrate the maximum bar height.", state.settings.mediaVisualizerPeakLevel, 1, 3)}
      ${settingToggle("mediaShowPauseOverlay", "Pause Cover Overlay", state.settings.mediaShowPauseOverlay)}
      ${settingToggle("mediaHideWhenInactive", "Hide Without Media", state.settings.mediaHideWhenInactive)}
      ${settingToggle("mediaAutoHidePaused", "Hide While Paused", state.settings.mediaAutoHidePaused)}
      ${settingToggle("mediaScrollingEnabled", "Scroll Long Titles", state.settings.mediaScrollingEnabled)}
      ${rangeSetting("mediaScrollingSpeed", "Title Scroll Speed", "Pixels per second.", state.settings.mediaScrollingSpeed, 1, 100, " px/s")}
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
  const permissions = permissionEntries(manifest.permissions);
  const fields = (manifest.settings || []).map((setting) => {
    const value = values[setting.key] ?? setting.default;
    if (setting.type === "boolean") return instanceToggle(setting.key, setting.label || setting.key, setting.description || "Widget setting", Boolean(value));
    if (setting.type === "number") return instanceNumberSetting(setting.key, setting.label || setting.key, setting.description || "Widget setting", value, setting.minimum ?? 0, setting.maximum ?? 1000000, setting.step ?? 1);
    if (setting.type === "select" && Array.isArray(setting.options)) return instanceSelectSetting(setting.key, setting.label || setting.key, setting.description || "Widget setting", value, setting.options.map((option) => [option.value, option.label || option.value]));
    return `<div class="setting-block"><div class="setting-head"><div><strong>${escapeHtml(setting.label || setting.key)}</strong><p>${escapeHtml(setting.description || "Widget setting")}</p></div></div><input class="text-input" type="${setting.type === "secret" ? "password" : "text"}" data-instance-setting="${escapeAttr(setting.key)}" value="${escapeAttr(value ?? "")}" /></div>`;
  });
  fields.unshift(`<div class="setting-row"><div><strong>Trust</strong><p>Local folders are not reviewed by the Taskbar Widgets registry.</p></div><span class="status-chip">Local / Unverified</span></div>`);
  if (permissions.length) fields.unshift(`<div class="setting-block"><div class="setting-head"><div><strong>Requested permissions</strong><p>${escapeHtml(permissions.map(permissionLabel).join(" · "))}</p></div></div></div>`);
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
  const current = update.currentVersion || "0.5.34";
  const latest = update.latestVersion || "Not checked";
  const checked = update.updatedAtUnix ? formatUnixTime(update.updatedAtUnix) : "Not checked";
  const isCurrent = update.state === "current" || (latest !== "Not checked" && latest.replace(/^v/i, "") === current.replace(/\.0$/, ""));
  return `
    ${pageHeader("updates")}
    <div class="updates-status fluent-card">
      <div class="status-orb ${isCurrent ? "ok" : "pending"}"><span class="material-symbols-outlined filled">${isCurrent ? "check_circle" : "new_releases"}</span></div>
      <div>
        <h3>${isCurrent ? "System is up to date" : update.updateAvailable ? "Update available" : "Release status"}</h3>
        <p>Checked: ${escapeHtml(checked)}</p>
      </div>
    </div>

    <div class="updates-layout">
      <section class="fluent-card channel-card">
        <h3>Update Channel</h3>
        <div class="segmented">
          <button class="active" type="button">Stable</button>
          <button disabled type="button">Dev</button>
        </div>
        <p>Currently on <strong>Stable</strong> channel. The Dev channel is not available in this build.</p>
      </section>

      <section class="fluent-card update-card">
        <div>
          <span class="status-chip">${update.updateAvailable ? "Available" : "Stable"}</span>
          <h3>${escapeHtml(latest)}</h3>
          <p>Current: ${escapeHtml(current)}</p>
        </div>
        <div class="update-actions">
          <button class="accent-action" id="check-updates" ${busy ? "disabled" : ""} type="button">
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

      <section class="fluent-card release-timeline">
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
    discordRealTimeVoiceEnabled: "Use the optional Windows helper for immediate speaking rings.",
    mediaDarkMode: "Use the modern dark media palette.",
    mediaShowControls: "Show native previous, play or pause, and next controls.",
    mediaShowVisualizer: "Draw FFT frequency bars from the active Windows output device.",
    mediaVisualizerCentered: "Grow frequency bars outward from the center line.",
    mediaVisualizerBaseline: "Draw a stable line below or through the visualizer.",
    mediaVisualizerBaselineAutoHide: "Hide the baseline when no audible signal is detected.",
    mediaShowPauseOverlay: "Show a pause glyph over the cover when playback is paused.",
    mediaHideWhenInactive: "Remove the media widget when no Windows media session is active.",
    mediaAutoHidePaused: "Temporarily remove the media widget while playback is paused.",
    mediaScrollingEnabled: "Scroll titles that do not fit in the available width.",
  };
  return hints[key] || "";
}

function mediaDiagnostics() {
  const media = state.mediaStatus || {};
  return `
    <div class="diagnostics-card">
      ${statusRow("Active", formatBool(media.active))}
      ${statusRow("Playing", formatBool(media.playing))}
      ${statusRow("Play / pause control", formatBool(media.canToggle))}
      ${statusRow("Previous control", formatBool(media.canPrevious))}
      ${statusRow("Next control", formatBool(media.canNext))}
      ${statusRow("Audio capture", formatBool(media.visualizerCaptureReady))}
      ${statusRow("Audible signal", formatBool(media.visualizerHasAudio))}
      ${statusRow("Visualizer sample rate", media.visualizerSampleRate ? `${media.visualizerSampleRate} Hz` : "Not available")}
      ${statusRow("Visualizer peak", Number.isFinite(Number(media.visualizerPeak)) ? Number(media.visualizerPeak).toFixed(3) : "Not available")}
      ${statusRow("Metadata source", media.metadataSource || "Not available")}
      ${statusRow("Source app", media.sourceApp || "Not available")}
      ${media.error ? statusRow("Error", media.error) : ""}
    </div>
  `;
}

function actionFooter() {
  return `
    <footer class="action-footer">
      <button class="accent-action" id="save-settings" type="button"><span class="material-symbols-outlined">save</span><span>Save Changes</span></button>
      <button class="secondary-action" id="open-packs" type="button"><span class="material-symbols-outlined">folder_open</span><span>Design Packs</span></button>
      ${inlineStatus()}
    </footer>
  `;
}

function inlineStatus() {
  return `<p id="inline-status" class="inline-status">${escapeHtml(state.status)}</p>`;
}

function renderFloatingTaskbar() {
  // Kept as a compatibility hook for settings input handlers.
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

async function openWidgetInstall(source, advertisedPermissions = null) {
  state.installSource = String(source || "");
  state.installAdvertisedPermissions = advertisedPermissions;
  state.installPreview = { loading: true };
  state.installError = "";
  state.installEnable = true;
  renderInstallModal();
  try {
    state.installPreview = await invoke("inspect_community_widget", {
      source: state.installSource,
      advertisedPermissions: state.installAdvertisedPermissions,
    });
    state.installOptionalGrants = permissionEntries(state.installPreview.permissions)
      .filter((request) => request.optional)
      .map((request) => request.id);
    if (state.installPreview.alreadyInstalled) {
      state.installEnable = isWidgetEnabled(state.installPreview.id);
    }
  } catch (error) {
    state.installPreview = null;
    state.installError = String(error);
  }
  renderInstallModal();
}

function installPermissionCard(request) {
  const info = permissionInfo(request);
  const optionalControl = request.optional
    ? `<label class="permission-optional-toggle"><input type="checkbox" data-optional-permission="${escapeAttr(request.id)}" ${state.installOptionalGrants.includes(request.id) ? "checked" : ""}/><span>Bu isteğe bağlı izne izin ver</span></label>`
    : `<span class="permission-required-badge">Gerekli</span>`;
  return `<li class="permission-card permission-risk-${escapeAttr(info.risk)}">
    <div class="permission-card-head"><strong>${escapeHtml(info.title)}</strong><span>${escapeHtml(info.risk === "critical" ? "Kritik" : info.risk === "high" ? "Yüksek risk" : info.risk === "medium" ? "Orta risk" : "Düşük risk")}</span></div>
    <p>${escapeHtml(info.description)}</p>
    ${info.scope ? `<div class="permission-scope"><span>Kapsam</span><code>${escapeHtml(info.scope)}</code></div>` : ""}
    ${request.reason ? `<div class="permission-reason"><span>Geliştiricinin nedeni</span><p>${escapeHtml(request.reason)}</p></div>` : ""}
    ${optionalControl}
  </li>`;
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
    root.innerHTML = `<div class="modal-backdrop"></div><section class="widget-modal install-modal fluent-card" role="dialog" aria-modal="true"><div class="install-loading"><span class="material-symbols-outlined">sync</span><strong>Inspecting widget package…</strong><p>Checking package paths, manifest, author and requested permissions.</p></div></section>`;
    localizeIcons();
    return;
  }
  if (state.installError) {
    root.innerHTML = `<div class="modal-backdrop" data-close-install></div><section class="widget-modal install-modal fluent-card" role="dialog" aria-modal="true"><header class="modal-head"><div><h3>Widget cannot be installed</h3><p>The package did not pass pre-install validation.</p></div><button class="icon-button" data-close-install type="button"><span class="material-symbols-outlined">close</span></button></header><div class="install-error"><span class="material-symbols-outlined">warning</span><p>${escapeHtml(state.installError)}</p></div><footer class="modal-actions"><button class="secondary-action" data-close-install type="button">Close</button></footer></section>`;
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
    <section class="widget-modal install-modal fluent-card" role="dialog" aria-modal="true" aria-label="${operation} ${escapeAttr(preview.displayName)}">
      <header class="modal-head">
        <div class="widget-title-block"><div class="widget-icon"><span class="material-symbols-outlined">extension</span></div><div><h3>${operation} ${escapeHtml(preview.displayName)}</h3><p>${escapeHtml(preview.id)} · ${preview.isUpdate ? `v${escapeHtml(preview.installedVersion)} → ` : ""}v${escapeHtml(preview.version)}</p></div></div>
        <button class="icon-button" data-close-install type="button"><span class="material-symbols-outlined">close</span></button>
      </header>
      <div class="install-scroll">
        <div class="install-summary">
          <p>${escapeHtml(preview.description)}</p>
          <div class="install-meta"><span>Author</span><strong>${escapeHtml(preview.authorName)}</strong><span>Provider</span><strong>${escapeHtml(preview.providerType)}</strong><span>Renderer</span><strong>${preview.rendererType === "web" ? "Legacy WebView2 UI" : preview.rendererType === "native" ? "Native XAML UI" : "Native DSL"}</strong><span>Çalışma seviyesi</span><strong>${preview.runAs === "administrator" ? "Administrator (UAC)" : "Windows kullanıcısı"}</strong><span>SHA-256</span><code>${escapeHtml(preview.packageSha256)}</code></div>
        </div>
        <div class="install-warning-stack">
          ${preview.rendererType === "web" ? `<div class="install-warning"><strong>Legacy web UI</strong><span>HTML, CSS ve JavaScript ayrı RenderHost içinde çalışır ve daha fazla kaynak kullanabilir.</span></div>` : ""}
          ${preview.providerType === "process" ? `<div class="install-warning install-warning-critical"><strong>Full-access process</strong><span>Bu paket ayrı bir executable veya script çalıştırır. Tam erişim kapsamı aşağıdaki kritik izinlerde açıklanır.</span></div>` : ""}
          ${preview.rendererChanged ? `<div class="install-warning"><strong>Renderer changed</strong><span>Bu güncelleme çalışma yüzeyini değiştiriyor ve yeniden onay gerektiriyor.</span></div>` : ""}
        </div>
        ${preview.executableFiles?.length ? `<div class="install-executables"><strong>Çalıştırılabilir içerik</strong><ul>${preview.executableFiles.map((file) => `<li><code>${escapeHtml(file)}</code></li>`).join("")}</ul></div>` : ""}
        <div class="permission-review">
          <div><span class="material-symbols-outlined">shield</span><div><h4>İstenen izinler</h4><p>Kurulumdan önce bu sürümün erişebileceği alanları inceleyin.</p></div></div>
          ${permissions.length ? `<ul class="permission-card-list">${permissions.map(installPermissionCard).join("")}</ul>` : `<p class="permission-none">Bu widget ek izin istemiyor.</p>`}
        </div>
        <label class="install-enable"><input id="enable-installed-widget" type="checkbox" ${state.installEnable ? "checked" : ""}/><span>${preview.isUpdate ? "Güncellemeden sonra etkin tut" : "Kurulumdan sonra etkinleştir"}</span></label>
        ${blockedExisting ? `<div class="install-warning">Version ${escapeHtml(preview.installedVersion || "unknown")} is already installed. Updates must have a higher version number.</div>` : ""}
      </div>
      <footer class="modal-actions">
        <button class="secondary-action" data-close-install type="button">Vazgeç</button>
        <button class="accent-action" id="confirm-widget-install" type="button" ${blockedExisting ? "disabled" : ""}><span class="material-symbols-outlined">${preview.isUpdate ? "sync" : "download"}</span><span>Onayla ve ${preview.isUpdate ? "güncelle" : "kur"}</span></button>
      </footer>
    </section>`;
  bindInstallModal();
  localizeIcons();
}

function closeInstallModal() {
  state.installPreview = null;
  state.installError = "";
  state.installSource = "";
  state.installAdvertisedPermissions = null;
  state.installOptionalGrants = [];
  renderInstallModal();
}

function bindInstallModal() {
  document.querySelectorAll("[data-close-install]").forEach((button) => {
    button.onclick = () => closeInstallModal();
  });
  document.getElementById("enable-installed-widget")?.addEventListener("change", (event) => {
    state.installEnable = event.target.checked;
  });
  document.querySelectorAll("[data-optional-permission]").forEach((input) => {
    input.addEventListener("change", () => {
      const permissionId = input.dataset.optionalPermission;
      state.installOptionalGrants = input.checked
        ? [...new Set([...state.installOptionalGrants, permissionId])]
        : state.installOptionalGrants.filter((id) => id !== permissionId);
    });
  });
  document.getElementById("confirm-widget-install")?.addEventListener("click", async (event) => {
    const button = event.currentTarget;
    button.disabled = true;
    const preview = state.installPreview;
    const enableAfter = state.installEnable;
    let runtimeUnloaded = false;
    try {
      if (preview?.isUpdate) {
        setStatus("Stopping the widget runtime for a safe update…");
        await invoke("control_runtime", { action: "unload" });
        runtimeUnloaded = true;
      }
      const id = await invoke("install_community_widget", {
        reviewToken: preview.reviewToken,
        grantedOptionalPermissions: state.installOptionalGrants,
        replaceExisting: Boolean(preview?.isUpdate),
      });
      if (runtimeUnloaded) {
        await invoke("control_runtime", { action: "load" });
        runtimeUnloaded = false;
      }
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
      if (runtimeUnloaded) {
        try { await invoke("control_runtime", { action: "load" }); } catch {}
      }
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
    <section class="widget-modal fluent-card" style="--accent:${catalog.accent}" role="dialog" aria-modal="true" aria-label="${escapeAttr(catalog.title)} settings">
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
        <button class="accent-action" id="save-widget-modal" type="button">
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
        <button class="accent-action" data-runtime-action="load" type="button">
          <span class="material-symbols-outlined">play_arrow</span>
          <span>Load</span>
        </button>
      </div>
      <p id="runtime-control-status"></p>
    </section>
  `;
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
  document.querySelectorAll(".widget-toggle-control").forEach((label) => {
    label.onclick = (event) => event.stopPropagation();
  });
  document.querySelectorAll("[data-toggle-widget]").forEach((control) => {
    control.onclick = (event) => {
      event.stopPropagation();
      const id = control.dataset.toggleWidget;
      const widget = widgetState(id);
      const manifest = widgetById(id);
      if (!widget.enabled && manifest?.local) {
        const requested = permissionEntries(manifest.permissions);
        if (requested.length) {
          const details = requested.map(permissionLabel).join("\n");
          if (!window.confirm(`This local/unverified widget requests:\n\n${details}\n\nEnable and approve these permissions?`)) {
            control.checked = false;
            return;
          }
          widget.settings._permissionsApproved = true;
        }
        if (!widget.settings._positionInitialized) {
          widget.positionPct = availableCommunityPosition(widget);
          widget.moveX = 0;
          widget.settings._positionInitialized = true;
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
  bindInputs(document.getElementById("page"));
  bindSequenceDragAndDrop();
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
  bindInputs(document.getElementById("widget-modal-root"), state.modalWidgetId);
  bindSystemMeterTabs();
  document.querySelectorAll("[data-close-modal]").forEach((button) => {
    button.onclick = () => closeWidgetModal();
  });
  bindRuntimeControls();
  bindDiscordVoiceHelper(document.getElementById("widget-modal-root"));
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
  bindInputs(document.getElementById("page"));
  bindWidgetButtons();
  bindSystemMeterTabs();
  bindRuntimeControls();
  bindDiscordVoiceHelper(document.getElementById("page"));
  document.getElementById("active-widget-select")?.addEventListener("change", (event) => {
    state.settings.activeDesign = event.target.value;
    render();
  });
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
      state.voiceHelperInstalled = Boolean(loaded.voiceHelperInstalled);
      setDirty(false);
      setStatus("Settings reloaded");
      render();
    } catch (error) {
      setStatus(`Reset failed: ${error}`);
    }
  });
}

function bindDiscordVoiceHelper(root = document) {
  root?.querySelectorAll("[data-install-voice-helper]").forEach((button) => button.addEventListener("click", async () => {
    state.voiceHelperBusy = true;
    setStatus("Waiting for Windows approval...");
    render();
    try {
      await invoke("install_voice_helper");
      if (!await waitForVoiceHelper(true)) {
        throw new Error("The helper was not installed. The Windows approval may have been cancelled.");
      }
      state.settings.discordRealTimeVoiceEnabled = true;
      setDirty(true);
      await saveSettings("Instant speaking detection enabled");
    } catch (error) {
      setStatus(`Voice helper setup failed: ${error}`);
    } finally {
      state.voiceHelperBusy = false;
      render();
    }
  }));

  root?.querySelectorAll("[data-remove-voice-helper]").forEach((button) => button.addEventListener("click", async () => {
    state.voiceHelperBusy = true;
    state.settings.discordRealTimeVoiceEnabled = false;
    await saveSettings("Instant speaking detection disabled");
    setStatus("Waiting for Windows approval...");
    render();
    try {
      await invoke("uninstall_voice_helper");
      if (!await waitForVoiceHelper(false)) {
        throw new Error("The helper is still installed. The Windows approval may have been cancelled.");
      }
      setStatus("Instant speaking helper removed");
    } catch (error) {
      setStatus(`Voice helper removal failed: ${error}`);
    } finally {
      state.voiceHelperBusy = false;
      render();
    }
  }));
}

async function waitForVoiceHelper(expected) {
  for (let attempt = 0; attempt < 60; attempt += 1) {
    await new Promise((resolve) => setTimeout(resolve, 500));
    const loaded = await invoke("load_state");
    state.voiceHelperInstalled = Boolean(loaded.voiceHelperInstalled);
    if (state.voiceHelperInstalled === expected) return true;
  }
  return false;
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

function bindInputs(root = document, instanceDesign = "") {
  root?.querySelectorAll("[data-color-target]").forEach((input) => {
    input.addEventListener("input", () => {
      const textInput = document.querySelector(`[data-instance-setting="${input.dataset.colorTarget}"]`);
      if (!textInput) return;
      textInput.value = input.value.toUpperCase();
      textInput.dispatchEvent(new Event("input", { bubbles: true }));
    });
  });
  root?.querySelectorAll("[data-setting]").forEach((input) => {
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
  root?.querySelectorAll("[data-widget-setting]").forEach((input) => {
    input.addEventListener("input", () => {
      const widget = instanceDesign ? widgetState(instanceDesign) : activeWidget();
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
  root?.querySelectorAll("[data-instance-setting]").forEach((input) => {
    input.addEventListener("input", () => {
      const widget = instanceDesign ? widgetState(instanceDesign) : activeWidget();
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
    state.voiceHelperInstalled = Boolean(loaded.voiceHelperInstalled);
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
