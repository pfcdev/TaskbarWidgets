(() => {
  "use strict";

  const elements = {
    clock: document.querySelector("#clock"),
    largeClock: document.querySelector("#large-clock"),
    compactState: document.querySelector("#compact-state"),
    compactTime: document.querySelector("#compact-time"),
    title: document.querySelector("#title"),
    remaining: document.querySelector("#remaining"),
    phase: document.querySelector("#phase"),
    ring: document.querySelector("#ring-value"),
    toggle: document.querySelector("#toggle"),
    reset: document.querySelector("#reset"),
    skip: document.querySelector("#skip")
  };

  const state = {
    phase: "focus",
    running: false,
    endAt: 0,
    remainingMs: 25 * 60_000,
    settings: {
      focusMinutes: 25,
      breakMinutes: 5,
      use24Hour: true,
      accentColor: "#8B5CF6"
    }
  };

  const phaseDuration = () =>
    (state.phase === "focus" ? state.settings.focusMinutes : state.settings.breakMinutes) * 60_000;

  function sanitizeSettings(value = {}) {
    state.settings.focusMinutes = Math.min(180, Math.max(1, Number(value.focusMinutes) || 25));
    state.settings.breakMinutes = Math.min(60, Math.max(1, Number(value.breakMinutes) || 5));
    state.settings.use24Hour = value.use24Hour !== false;
    state.settings.accentColor = /^#[0-9a-f]{6}$/i.test(value.accentColor || "")
      ? value.accentColor
      : "#8B5CF6";
    document.documentElement.style.setProperty("--accent", state.settings.accentColor);
    if (!state.running) state.remainingMs = Math.min(state.remainingMs, phaseDuration());
  }

  function formatDuration(milliseconds) {
    const seconds = Math.max(0, Math.ceil(milliseconds / 1000));
    return `${String(Math.floor(seconds / 60)).padStart(2, "0")}:${String(seconds % 60).padStart(2, "0")}`;
  }

  function tick() {
    const now = Date.now();
    if (state.running) {
      state.remainingMs = Math.max(0, state.endAt - now);
      if (state.remainingMs === 0) changePhase();
    }
    const clock = new Intl.DateTimeFormat(undefined, {
      hour: "2-digit",
      minute: "2-digit",
      hour12: !state.settings.use24Hour
    }).format(now);
    const remaining = formatDuration(state.remainingMs);
    const progress = 1 - state.remainingMs / Math.max(1, phaseDuration());
    const phaseLabel = state.phase === "focus" ? "Focus" : "Break";

    elements.clock.textContent = clock;
    elements.largeClock.textContent = clock;
    elements.compactState.textContent = state.running ? phaseLabel : "Ready";
    elements.compactTime.textContent = remaining;
    elements.remaining.textContent = remaining;
    elements.phase.textContent = `${phaseLabel} session`;
    elements.title.textContent = state.running
      ? (state.phase === "focus" ? "Stay in the moment" : "Take a breath")
      : "Ready to focus";
    elements.toggle.textContent = state.running ? "Pause" : `Start ${phaseLabel.toLowerCase()}`;
    elements.ring.style.strokeDashoffset = String(270.18 * Math.min(1, Math.max(0, progress)));
  }

  async function save() {
    await window.taskbarWidget.storage.set("timer", {
      phase: state.phase,
      running: state.running,
      endAt: state.endAt,
      remainingMs: state.remainingMs
    });
  }

  async function restore() {
    const stored = await window.taskbarWidget.storage.get("timer");
    if (!stored || !["focus", "break"].includes(stored.phase)) return;
    state.phase = stored.phase;
    state.running = stored.running === true;
    state.endAt = Number(stored.endAt) || 0;
    state.remainingMs = Math.min(
      phaseDuration(),
      Math.max(0, Number(stored.remainingMs) || phaseDuration())
    );
    if (state.running) state.remainingMs = Math.max(0, state.endAt - Date.now());
  }

  function changePhase() {
    state.phase = state.phase === "focus" ? "break" : "focus";
    state.running = false;
    state.endAt = 0;
    state.remainingMs = phaseDuration();
    void save();
    tick();
  }

  elements.toggle.addEventListener("click", () => {
    state.running = !state.running;
    state.endAt = state.running ? Date.now() + state.remainingMs : 0;
    void save();
    tick();
  });

  elements.reset.addEventListener("click", () => {
    state.running = false;
    state.endAt = 0;
    state.remainingMs = phaseDuration();
    void save();
    tick();
  });

  elements.skip.addEventListener("click", changePhase);

  window.taskbarWidget.on("snapshot", payload => {
    sanitizeSettings(payload?.settings);
    tick();
  });
  window.taskbarWidget.on("settings", sanitizeSettings);
  window.taskbarWidget.on("lifecycle", value => {
    document.body.dataset.surface = value?.state === "expanded" ? "expanded" : "collapsed";
  });

  void restore().finally(() => {
    tick();
    setInterval(tick, 1000);
    window.taskbarWidget.ready();
  });
})();
