const { invoke } = window.__TAURI__.core;

const designs = [
  {
    id: "codex-status",
    title: "Codex Status",
    subtitle: "API Quota Monitor",
    accentClass: "cyan",
    description:
      "Displays Codex quota, active project state, and account information in a compact taskbar capsule.",
  },
  {
    id: "weather-static",
    title: "Static Weather",
    subtitle: "Weather Capsule",
    accentClass: "amber",
    description:
      "Shows current weather for the selected city with the custom weather taskbar design.",
  },
  {
    id: "discord-voice",
    title: "Discord Voice",
    subtitle: "Live Voice Avatars",
    accentClass: "green",
    description:
      "Shows Discord voice users with dimmed idle avatars and a green speaking frame.",
  },
  {
    id: "btc-fees",
    title: "Crypto Fees",
    subtitle: "Fee Capsule",
    accentClass: "pink",
    description:
      "A compact crypto fee design surface for taskbar-sized market and network data.",
  },
  {
    id: "media-player",
    title: "Media Player",
    subtitle: "System Media",
    accentClass: "violet",
    description:
      "Reads Windows media sessions and shows title, artist, cover, and play/pause state.",
  },
  {
    id: "steam-download",
    title: "Steam Downloads",
    subtitle: "Download Progress",
    accentClass: "blue",
    description:
      "Shows the active Steam download with game art, progress, speed, and remaining time.",
  },
];

const defaultWidgets = designs.map((design, index) => ({
  id: design.id,
  design: design.id,
  enabled: design.id === "codex-status",
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
  rotationDesigns: designs.map((item) => item.id),
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

let state = {
  appDir: "",
  page: "library",
  settings: { ...defaults },
  updateStatus: {},
  mediaStatus: {},
  dirty: false,
  status: "",
};
let autosaveTimer = 0;

function designById(id) {
  return designs.find((item) => item.id === id) || designs[0];
}

function mergeSettings(settings) {
  const merged = { ...defaults, ...settings };
  merged.activeDesign = designById(merged.activeDesign).id;
  merged.rotationDesigns = normalizeRotation(merged.rotationDesigns);
  merged.widgetOffsetPx = clampNumber(merged.widgetOffsetPx, 0, 480, 0);
  if (settings && Object.prototype.hasOwnProperty.call(settings, "widgetMoveX")) {
    merged.widgetMoveX = clampNumber(merged.widgetMoveX, -640, 640, 0);
  } else {
    merged.widgetMoveX = -clampNumber(merged.widgetOffsetPx, 0, 480, 0);
  }
  merged.widgets = normalizeWidgets(merged.widgets, merged.activeDesign, merged.enabled, merged.widgetMoveX);
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
    const design = designById(item.design || item.designId || item.id).id;
    if (result.some((widget) => widget.design === design)) continue;
    result.push({
      id: item.id || design,
      design,
      enabled: Boolean(item.enabled),
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

function currentWidget() {
  let widget = state.settings.widgets.find((item) => item.design === state.settings.activeDesign);
  if (!widget) {
    widget = { id: state.settings.activeDesign, design: state.settings.activeDesign, enabled: false, moveX: 0, positionPct: 100, order: state.settings.widgets.length };
    state.settings.widgets.push(widget);
  }
  return widget;
}

function normalizeRotation(list) {
  const result = [];
  const source = Array.isArray(list) && list.length ? list : defaults.rotationDesigns;
  for (const id of source) {
    const normalized = designById(id).id;
    if (!result.includes(normalized)) result.push(normalized);
  }
  return result.length ? result : ["codex-status"];
}

function clampNumber(value, min, max, fallback) {
  const number = Number.parseInt(value, 10);
  if (!Number.isFinite(number)) return fallback;
  return Math.min(max, Math.max(min, number));
}

function setDirty(dirty = true) {
  state.dirty = dirty;
  document.getElementById("save-state").textContent = dirty ? "Unsaved" : "Saved";
  document.getElementById("save-state").classList.toggle("dirty", dirty);
}

function setStatus(message) {
  state.status = message || "";
  const status = document.getElementById("inline-status");
  if (status) status.textContent = state.status;
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
}

function render() {
  renderNavigation();
  renderWidgetList();
  renderPage();
  setDirty(state.dirty);
}

function renderNavigation() {
  document.querySelectorAll(".nav-item").forEach((button) => {
    button.classList.toggle("active", button.dataset.page === state.page);
    button.onclick = () => {
      state.page = button.dataset.page;
      render();
    };
  });
}

function renderWidgetList() {
  const list = document.getElementById("widget-list");
  list.innerHTML = "";
  for (const design of designs) {
    const button = document.createElement("button");
    button.className = `widget-card ${design.id === state.settings.activeDesign ? "active" : ""}`;
    button.innerHTML = `
      <span class="dot ${design.accentClass}"></span>
      <span>
        <strong>${escapeHtml(design.title)}</strong>
        <small>${currentWidgetState(design.id)} - ${escapeHtml(design.subtitle)}</small>
      </span>
    `;
    button.onclick = () => {
      state.page = "library";
      state.settings.activeDesign = design.id;
      setDirty(true);
      render();
    };
    list.appendChild(button);
  }
}

function currentWidgetState(designId) {
  const widget = state.settings.widgets.find((item) => item.design === designId);
  return widget?.enabled ? "On" : "Off";
}

function renderPage() {
  const active = designById(state.settings.activeDesign);
  const title = document.getElementById("page-title");
  const eyebrow = document.getElementById("eyebrow");
  const page = document.getElementById("page");
  eyebrow.textContent = pageLabel();

  if (state.page === "rotation") {
    title.textContent = "Slider Rotation";
    page.innerHTML = rotationTemplate();
    bindRotation();
    return;
  }

  if (state.page === "updates") {
    title.textContent = "Updates";
    page.innerHTML = updatesTemplate();
    bindUpdates();
    return;
  }

  title.textContent = active.title;
  page.innerHTML = libraryTemplate(active);
  bindLibrary(active);
}

function pageLabel() {
  if (state.page === "rotation") return "Slider Rotation";
  if (state.page === "updates") return "Release Management";
  return "Widget Settings";
}

function libraryTemplate(design) {
  const widget = currentWidget();
  return `
    <div class="grid">
      <div class="stack">
        <section class="panel preview">
          <div class="taskbar-strip">${previewMarkup(design.id)}</div>
        </section>
        <section class="panel">
          <h3>${escapeHtml(design.title)}</h3>
          <p>${escapeHtml(design.description)}</p>
        </section>
      </div>

      <div class="stack">
        <section class="panel">
          <h3>General Settings</h3>
          ${widgetSwitchField("enabled", "Widget Enabled", "Show this widget on the taskbar", widget.enabled)}
          ${numberField("refreshIntervalSecs", "Refresh Interval", "Update frequency in seconds", state.settings.refreshIntervalSecs, 1, 3600)}
          ${widgetRangeField("positionPct", "Move", "0% is taskbar left, 100% is before the system tray", widget.positionPct ?? 100, 0, 100, "%")}
        </section>

        <section class="panel">
          <h3>${escapeHtml(design.title)} Settings</h3>
          ${widgetSpecificFields(design.id)}
        </section>

        ${actionsTemplate()}
      </div>
    </div>
  `;
}

function previewMarkup(id) {
  if (id === "weather-static") {
    return `<div class="capsule weather-capsule"><span><strong>Ceyhan, Adana</strong><small>23:32 - 12/07</small></span><strong>26 deg</strong></div>`;
  }
  if (id === "discord-voice") {
    return `<div class="capsule discord-capsule"><span class="avatar"></span><span class="avatar speaking"></span><span class="avatar"></span><span class="avatar"></span></div>`;
  }
  if (id === "media-player") {
    return `<div class="capsule media-capsule"><span class="cover"></span><span><strong>Now Playing</strong><small>System media</small></span><span class="play">></span></div>`;
  }
  if (id === "steam-download") {
    return `<div class="capsule steam-capsule"><span class="cover steam-cover"></span><span><strong>Game Download</strong><small>8dk kaldi</small></span><span class="steam-progress"><strong>42%</strong><i></i></span></div>`;
  }
  if (id === "btc-fees") {
    return `<div class="capsule"><strong>ETH Fees</strong><small>Base 12 gwei - Priority 2</small></div>`;
  }
  return `<div class="capsule"><strong>Antigravity</strong><small>Quota and tasks</small></div>`;
}

function widgetSpecificFields(id) {
  if (id === "weather-static") {
    return `
      ${textField("weatherCity", "City", "Weather location name", state.settings.weatherCity, "Istanbul")}
      ${selectField("weatherTempUnit", "Temperature Unit", "Display format for temperature", state.settings.weatherTempUnit, [["C", "Celsius"], ["F", "Fahrenheit"]])}
    `;
  }
  if (id === "discord-voice") {
    return `
      ${switchField("discordEnabled", "Discord Integration", "Read selected voice channel users from Discord", state.settings.discordEnabled)}
      ${switchField("discordBackgroundEnabled", "Widget Background", "Show the black capsule behind avatars", state.settings.discordBackgroundEnabled)}
      ${textField("discordClientId", "Client ID", "Discord application client id", state.settings.discordClientId, "1525972653641433288")}
      ${textField("discordClientSecret", "Client Secret", "Stored in widget-settings.json", state.settings.discordClientSecret, "client secret", true)}
      ${textField("discordRedirectUri", "Redirect URI", "Must match Discord Developer Portal", state.settings.discordRedirectUri, "http://127.0.0.1/callback")}
    `;
  }
  if (id === "media-player") {
    return `
      ${switchField("mediaDarkMode", "Dark Mode", "Use the modern dark media palette", state.settings.mediaDarkMode)}
      ${mediaDiagnosticsTemplate()}
    `;
  }
  if (id === "steam-download") {
    return `
      <div class="field">
        <label>Steam Source</label>
        <small>Reads Steam library manifests and content logs from the local Steam installation.</small>
      </div>
    `;
  }
  return `
    ${textField("codexApiEndpoint", "API Endpoint", "Custom API endpoint URL", state.settings.codexApiEndpoint, "https://api.example.com")}
    ${textField("codexProjectFilter", "Project Filter", "Filter displayed projects by name", state.settings.codexProjectFilter, "my-project")}
  `;
}

function switchField(key, label, hint, checked) {
  return `
    <div class="field row">
      <div>
        <label>${escapeHtml(label)}</label>
        <small>${escapeHtml(hint)}</small>
      </div>
      <label class="switch">
        <input type="checkbox" data-setting="${key}" ${checked ? "checked" : ""} />
        <span></span>
      </label>
    </div>
  `;
}

function widgetSwitchField(key, label, hint, checked) {
  return `
    <div class="field row">
      <div>
        <label>${escapeHtml(label)}</label>
        <small>${escapeHtml(hint)}</small>
      </div>
      <label class="switch">
        <input type="checkbox" data-widget-setting="${key}" ${checked ? "checked" : ""} />
        <span></span>
      </label>
    </div>
  `;
}

function numberField(key, label, hint, value, min, max) {
  return `
    <div class="field">
      <label>${escapeHtml(label)}</label>
      <small>${escapeHtml(hint)}</small>
      <input type="number" min="${min}" max="${max}" data-setting="${key}" value="${escapeAttr(value)}" />
    </div>
  `;
}

function rangeField(key, label, hint, value, min, max, unit = "") {
  const suffix = unit ? ` ${unit}` : "";
  return `
    <div class="field">
      <div class="row">
        <div>
          <label>${escapeHtml(label)}</label>
          <small>${escapeHtml(hint)}</small>
        </div>
        <strong id="${key}-value">${value}${suffix}</strong>
      </div>
      <input type="range" min="${min}" max="${max}" step="4" data-setting="${key}" data-unit="${escapeAttr(unit)}" value="${escapeAttr(value)}" />
    </div>
  `;
}

function widgetRangeField(key, label, hint, value, min, max, unit = "") {
  const suffix = unit ? ` ${unit}` : "";
  return `
    <div class="field">
      <div class="row">
        <div>
          <label>${escapeHtml(label)}</label>
          <small>${escapeHtml(hint)}</small>
        </div>
        <strong id="widget-${key}-value">${value}${suffix}</strong>
      </div>
      <input type="range" min="${min}" max="${max}" step="4" data-widget-setting="${key}" data-unit="${escapeAttr(unit)}" value="${escapeAttr(value)}" />
    </div>
  `;
}

function textField(key, label, hint, value, placeholder, secret = false) {
  return `
    <div class="field">
      <label>${escapeHtml(label)}</label>
      <small>${escapeHtml(hint)}</small>
      <input type="${secret ? "password" : "text"}" data-setting="${key}" value="${escapeAttr(value || "")}" placeholder="${escapeAttr(placeholder)}" />
    </div>
  `;
}

function selectField(key, label, hint, value, options) {
  return `
    <div class="field">
      <label>${escapeHtml(label)}</label>
      <small>${escapeHtml(hint)}</small>
      <select data-setting="${key}">
        ${options.map(([id, text]) => `<option value="${escapeAttr(id)}" ${id === value ? "selected" : ""}>${escapeHtml(text)}</option>`).join("")}
      </select>
    </div>
  `;
}

function actionsTemplate() {
  return `
    <section class="actions">
      <button class="btn primary" id="save">Save Changes</button>
      <button class="btn" id="reset">Reset</button>
      <button class="btn" id="packs">Design Packs</button>
    </section>
    <p id="inline-status">${escapeHtml(state.status)}</p>
  `;
}

function bindLibrary() {
  bindSettingInputs();
  bindDiagnostics();
  bindActions();
}

function bindDiagnostics() {
  const refresh = document.getElementById("refresh-media-diagnostics");
  if (!refresh) return;
  refresh.onclick = async () => {
    try {
      const loaded = await invoke("load_state");
      state.mediaStatus = loaded.mediaStatus || {};
      setStatus("Runtime diagnostics refreshed");
      render();
    } catch (error) {
      setStatus(`Refresh failed: ${error}`);
    }
  };
}

function bindSettingInputs() {
  document.querySelectorAll("[data-setting]").forEach((input) => {
    input.addEventListener("input", () => {
      const key = input.dataset.setting;
      if (input.type === "checkbox") {
        state.settings[key] = input.checked;
      } else if (input.type === "number" || input.type === "range") {
        state.settings[key] = clampNumber(input.value, Number(input.min || 0), Number(input.max || 3600), defaults[key] || 0);
        if (input.type === "range") {
          const value = document.getElementById(`${key}-value`);
          const unit = input.dataset.unit ? ` ${input.dataset.unit}` : "";
          if (value) value.textContent = `${state.settings[key]}${unit}`;
        }
      } else {
        state.settings[key] = input.value;
      }
      setDirty(true);
      setStatus("");
    });
  });
  document.querySelectorAll("[data-widget-setting]").forEach((input) => {
    input.addEventListener("input", () => {
      const widget = currentWidget();
      const key = input.dataset.widgetSetting;
      if (input.type === "checkbox") {
        widget[key] = input.checked;
      } else if (input.type === "number" || input.type === "range") {
        widget[key] = clampNumber(input.value, Number(input.min || -640), Number(input.max || 640), widget[key] || 0);
        if (input.type === "range") {
          const value = document.getElementById(`widget-${key}-value`);
          const unit = input.dataset.unit ? ` ${input.dataset.unit}` : "";
          if (value) value.textContent = `${widget[key]}${unit}`;
        }
      } else {
        widget[key] = input.value;
      }
      setDirty(true);
      setStatus("");
      renderWidgetList();
      scheduleAutosave();
    });
  });
}

function scheduleAutosave() {
  clearTimeout(autosaveTimer);
  autosaveTimer = setTimeout(async () => {
    await saveSettings("Applied");
  }, 350);
}

function bindActions() {
  const save = document.getElementById("save");
  if (save) save.onclick = () => saveSettings();
  const reset = document.getElementById("reset");
  if (reset) reset.onclick = async () => {
    const loaded = await invoke("load_state");
    state.settings = mergeSettings(loaded.settings || {});
    state.updateStatus = loaded.updateStatus || {};
    state.mediaStatus = loaded.mediaStatus || {};
    setDirty(false);
    setStatus("Settings reset");
    render();
  };
  const packs = document.getElementById("packs");
  if (packs) packs.onclick = async () => {
    try {
      await invoke("open_widget_libraries");
      setStatus("WidgetLibraries opened");
    } catch (error) {
      setStatus(`Open failed: ${error}`);
    }
  };
}

async function saveSettings(successMessage = "Settings saved") {
  try {
    state.settings.rotationDesigns = normalizeRotation(state.settings.rotationDesigns);
    state.settings.widgets = normalizeWidgets(state.settings.widgets, state.settings.activeDesign, state.settings.enabled, state.settings.widgetMoveX);
    const widget = currentWidget();
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

function rotationTemplate() {
  return `
    <div class="grid">
      <section class="panel">
        <h3>Rotation Controls</h3>
        ${switchField("rotationEnabled", "Auto Rotate Widgets", "Cycle through selected widgets in order", state.settings.rotationEnabled)}
        ${numberField("rotationIntervalSecs", "Slide Interval", "Seconds before switching to the next widget", state.settings.rotationIntervalSecs, 5, 3600)}
      </section>
      <section class="panel">
        <h3>Rotation Queue</h3>
        <p>Choose widgets and arrange the exact sequence.</p>
        <div class="queue">
          ${designs.map(queueItemTemplate).join("")}
        </div>
      </section>
    </div>
    ${actionsTemplate()}
  `;
}

function queueItemTemplate(design) {
  const index = state.settings.rotationDesigns.indexOf(design.id);
  const enabled = index >= 0;
  return `
    <div class="queue-item">
      <input type="checkbox" data-queue="${design.id}" ${enabled ? "checked" : ""} />
      <div class="queue-title">
        <strong>${escapeHtml(design.title)}</strong>
        <small>${enabled ? `#${index + 1}` : "Off"} - ${escapeHtml(design.subtitle)}</small>
      </div>
      <div class="mini-actions">
        <button data-move="${design.id}" data-dir="-1" ${!enabled || index === 0 ? "disabled" : ""}>Up</button>
        <button data-move="${design.id}" data-dir="1" ${!enabled || index === state.settings.rotationDesigns.length - 1 ? "disabled" : ""}>Dn</button>
      </div>
    </div>
  `;
}

function bindRotation() {
  bindSettingInputs();
  document.querySelectorAll("[data-queue]").forEach((checkbox) => {
    checkbox.onchange = () => {
      const id = checkbox.dataset.queue;
      if (checkbox.checked) {
        if (!state.settings.rotationDesigns.includes(id)) state.settings.rotationDesigns.push(id);
      } else {
        state.settings.rotationDesigns = state.settings.rotationDesigns.filter((item) => item !== id);
        if (!state.settings.rotationDesigns.length) state.settings.rotationDesigns = [id];
      }
      setDirty(true);
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
        [state.settings.rotationDesigns[index], state.settings.rotationDesigns[next]] = [state.settings.rotationDesigns[next], state.settings.rotationDesigns[index]];
        setDirty(true);
        render();
      }
    };
  });
  bindActions();
}

function updatesTemplate() {
  const update = state.updateStatus || {};
  const busy = update.state === "checking" || update.state === "downloading" || update.state === "installing";
  return `
    <div class="grid">
      <section class="panel">
        <h3>Release Status</h3>
        <div class="status-table">
          ${statusRow("Current version", update.currentVersion || "0.1.0")}
          ${statusRow("Latest release", update.latestVersion || "Not checked")}
          ${statusRow("State", update.state || "idle")}
          ${statusRow("Updated", update.updatedAtUnix ? formatUnixTime(update.updatedAtUnix) : "Not checked")}
        </div>
        <p class="${busy ? "pulse" : ""}">${escapeHtml(update.message || "Run a check to refresh update status.")}</p>
      </section>
      <section class="panel">
        <h3>Actions</h3>
        <div class="actions">
          <button class="btn primary" id="check-updates" ${busy ? "disabled" : ""}>${busy ? "Checking..." : "Check Updates"}</button>
          <button class="btn ${update.updateAvailable ? "success" : ""}" id="install-update" ${!update.updateAvailable || busy ? "disabled" : ""}>Install Update</button>
        </div>
        <p id="inline-status">${escapeHtml(state.status)}</p>
      </section>
    </div>
  `;
}

function mediaDiagnosticsTemplate() {
  const media = state.mediaStatus || {};
  return `
    <div class="diagnostics">
      <div class="row diagnostics-head">
        <h4>Runtime Diagnostics</h4>
        <button class="mini-btn" id="refresh-media-diagnostics">Refresh</button>
      </div>
      <div class="status-table compact">
        ${statusRow("Active", formatBool(media.active))}
        ${statusRow("Playing", formatBool(media.playing))}
        ${statusRow("Metadata source", media.metadataSource || "Not available")}
        ${statusRow("Sessions", media.sessionCount ?? "0")}
        ${statusRow("Source app", media.sourceApp || "Not available")}
        ${statusRow("Title", media.title || "Not available")}
        ${statusRow("Artist", media.artist || "Not available")}
        ${statusRow("Updated", media.updatedAtUnix ? formatUnixTime(media.updatedAtUnix) : "Not available")}
        ${media.error ? statusRow("Error", media.error) : ""}
      </div>
    </div>
  `;
}

function formatBool(value) {
  if (value === true) return "Yes";
  if (value === false) return "No";
  return "Unknown";
}

function statusRow(label, value) {
  return `<div class="status-row"><span>${escapeHtml(label)}</span><strong>${escapeHtml(value)}</strong></div>`;
}

function bindUpdates() {
  document.getElementById("check-updates").onclick = async () => {
    try {
      setStatus("Checking for updates...");
      state.updateStatus = { ...state.updateStatus, state: "checking", message: "Checking GitHub latest release..." };
      render();
      const loaded = await invoke("run_loader_command", { arg: "--check-updates" });
      state.updateStatus = loaded.updateStatus || {};
      setStatus(state.updateStatus.message || "Update check finished");
      render();
    } catch (error) {
      setStatus(`Update check failed: ${error}`);
      await refreshState();
    }
  };
  document.getElementById("install-update").onclick = async () => {
    try {
      setStatus("Downloading update...");
      state.updateStatus = { ...state.updateStatus, state: "downloading", message: "Downloading update..." };
      render();
      const loaded = await invoke("run_loader_command", { arg: "--update" });
      state.updateStatus = loaded.updateStatus || {};
      setStatus(state.updateStatus.message || "Update command finished");
      render();
    } catch (error) {
      setStatus(`Update install failed: ${error}`);
      await refreshState();
    }
  };
}

function formatUnixTime(value) {
  const seconds = Number(value);
  if (!Number.isFinite(seconds) || seconds <= 0) return "Not checked";
  return new Date(seconds * 1000).toLocaleString();
}

async function refreshState() {
  try {
    const loaded = await invoke("load_state");
    state.updateStatus = loaded.updateStatus || {};
    state.mediaStatus = loaded.mediaStatus || {};
    render();
  } catch {
    // Keep the visible status from the attempted action.
  }
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
