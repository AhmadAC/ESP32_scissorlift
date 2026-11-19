let servoAngle = 90;
let savedMinAngle = 0;
let savedMaxAngle = 180;
let draftMinAngle = 0;
let draftMaxAngle = 180;
let liftPercent = 50;

let servoEnabled = false;
let autoIdleEnabled = false;
let joyInvertEnabled = false;
let servoName = "MG996R";

let isDraggingSlider = false;
let lastSliderEditMs = 0;
let pendingServoCmd = null;
let servoThrottleTimer = null;

function initServoStorage() {
  try {
    const storedName = localStorage.getItem("mg996r_servo_name");
    if (storedName && storedName.trim().length > 0) {
      servoName = storedName.trim().substring(0, 28);
    }
    const storedRange = localStorage.getItem("mg996r_servo_range");
    if (storedRange) {
      const parsed = JSON.parse(storedRange);
      if (parsed && typeof parsed.min === "number" && typeof parsed.max === "number") {
        const mn = Math.max(0, Math.min(179, Math.round(parsed.min)));
        const mx = Math.max(mn + 1, Math.min(180, Math.round(parsed.max)));
        savedMinAngle = mn;
        savedMaxAngle = mx;
        draftMinAngle = mn;
        draftMaxAngle = mx;
      }
    }
  } catch (e) {}

  liftPercent = angleToPercent(servoAngle, savedMinAngle, savedMaxAngle);
  applyAutoIdleState(autoIdleEnabled);
  applyJoystickInvertState(joyInvertEnabled);
  updateServoUI();
}

function percentToAngle(pct, minA, maxA) {
  const clampedPct = Math.max(0, Math.min(100, pct));
  const span = Math.max(1, maxA - minA);
  return Math.round(minA + (clampedPct / 100) * span);
}

function angleToPercent(angle, minA, maxA) {
  const span = Math.max(1, maxA - minA);
  const pct = Math.round(((angle - minA) / span) * 100);
  return Math.max(0, Math.min(100, pct));
}

function updateServoStateBadge() {
  const badge = document.getElementById("s1-state-badge");
  if (!badge) return;
  if (servoEnabled) {
    badge.innerText = "ACTIVE";
    badge.style.background = "rgba(16,185,129,0.2)";
    badge.style.color = "#34d399";
    badge.style.borderColor = "rgba(16,185,129,0.4)";
  } else {
    badge.innerText = "IDLE";
    badge.style.background = "rgba(148,163,184,0.15)";
    badge.style.color = "#94a3b8";
    badge.style.borderColor = "rgba(148,163,184,0.3)";
  }
}

function updateTrimDirtyIndicator() {
  const badge = document.getElementById("trim-sync-badge");
  if (!badge) return;
  const dirty = (draftMinAngle !== savedMinAngle) || (draftMaxAngle !== savedMaxAngle);
  if (dirty) {
    badge.innerText = "Unsaved";
    badge.style.background = "rgba(245,158,11,0.22)";
    badge.style.color = "#fbbf24";
  } else {
    badge.innerText = "Saved";
    badge.style.background = "rgba(16,185,129,0.18)";
    badge.style.color = "#34d399";
  }
}

function updateServoUI() {
  const sl = document.getElementById("s1-slider");
  const hdr = document.getElementById("s1-header");
  const rangeBadge = document.getElementById("saved-range-badge");

  if (sl) sl.value = liftPercent;
  if (hdr) {
    hdr.innerHTML = `${escapeHtml(servoName)}: <span id="s1-val" style="color:#38bdf8; font-family:monospace; font-weight:700;">${liftPercent}% (${servoAngle}°)</span>`;
  }
  if (rangeBadge) {
    rangeBadge.innerText = `Range: ${savedMinAngle}° – ${savedMaxAngle}°`;
  }

  const qMin = document.getElementById("quick-min-lbl");
  const qMid = document.getElementById("quick-mid-lbl");
  const qMax = document.getElementById("quick-max-lbl");
  const midAngle = Math.round((savedMinAngle + savedMaxAngle) / 2);
  if (qMin) qMin.innerText = `Min (${savedMinAngle}°)`;
  if (qMid) qMid.innerText = `Mid (${midAngle}°)`;
  if (qMax) qMax.innerText = `Max (${savedMaxAngle}°)`;

  const minValEl = document.getElementById("min-trim-val");
  const maxValEl = document.getElementById("max-trim-val");
  const minSl = document.getElementById("min-trim-slider");
  const maxSl = document.getElementById("max-trim-slider");

  if (minValEl) minValEl.innerText = `${draftMinAngle}°`;
  if (maxValEl) maxValEl.innerText = `${draftMaxAngle}°`;
  if (minSl) minSl.value = draftMinAngle;
  if (maxSl) maxSl.value = draftMaxAngle;

  updateServoStateBadge();
  updateTrimDirtyIndicator();
}

function editServoName() {
  const next = prompt("Rename Servo:", servoName || "MG996R");
  if (next && next.trim().length > 0) {
    servoName = next.trim().substring(0, 28);
    try { localStorage.setItem("mg996r_servo_name", servoName); } catch (e) {}
    updateServoUI();
  }
}

function toggleAutoIdle() {
  autoIdleEnabled = !autoIdleEnabled;
  applyAutoIdleState(autoIdleEnabled);
  sendCommand(`autoidle ${autoIdleEnabled ? "on" : "off"}`);
  showToast(autoIdleEnabled ? "Auto-Idle ON (relaxes after 3s)" : "Auto-Idle OFF (holds torque)", "info");
}

function applyAutoIdleState(enabled) {
  autoIdleEnabled = !!enabled;
  const btn = document.getElementById("auto-idle-btn");
  if (!btn) return;
  if (autoIdleEnabled) {
    btn.innerText = "Auto-Idle: ON";
    btn.style.background = "#10b981";
    btn.style.color = "#000000";
    btn.style.borderColor = "#10b981";
  } else {
    btn.innerText = "Auto-Idle: OFF";
    btn.style.background = "#1e293b";
    btn.style.color = "rgba(255,255,255,0.75)";
    btn.style.borderColor = "#475569";
  }
}

function toggleJoystickInvert() {
  joyInvertEnabled = !joyInvertEnabled;
  applyJoystickInvertState(joyInvertEnabled);
  sendCommand(`jinvert ${joyInvertEnabled ? "on" : "off"}`);
  showToast(joyInvertEnabled ? "Joystick Invert ON (directions flipped)" : "Joystick Invert OFF (normal)", "info");
}

function applyJoystickInvertState(enabled) {
  joyInvertEnabled = !!enabled;
  const btn = document.getElementById("joy-invert-btn");
  const txt = document.getElementById("joy-invert-text");
  if (!btn || !txt) return;
  if (joyInvertEnabled) {
    txt.innerText = "Joystick Invert: ON";
    btn.style.background = "#a855f7";
    btn.style.color = "#ffffff";
    btn.style.borderColor = "#c084fc";
  } else {
    txt.innerText = "Joystick Invert: OFF";
    btn.style.background = "#1e293b";
    btn.style.color = "rgba(255,255,255,0.8)";
    btn.style.borderColor = "#475569";
  }
}

function queueThrottledAngle(angle) {
  pendingServoCmd = `s1 ${angle}`;
  if (!servoThrottleTimer) {
    servoThrottleTimer = setTimeout(() => {
      servoThrottleTimer = null;
      const cmd = pendingServoCmd;
      pendingServoCmd = null;
      if (cmd) sendCommand(cmd);
    }, 80);
  }
}

function onSliderStart() {
  isDraggingSlider = true;
  lastSliderEditMs = Date.now();
}

function onServoSlider(pctVal) {
  const parsedPct = parseInt(pctVal, 10);
  if (isNaN(parsedPct)) return;

  lastSliderEditMs = Date.now();
  servoEnabled = true;
  liftPercent = Math.max(0, Math.min(100, parsedPct));
  servoAngle = percentToAngle(liftPercent, savedMinAngle, savedMaxAngle);

  const valEl = document.getElementById("s1-val");
  if (valEl) valEl.innerText = `${liftPercent}% (${servoAngle}°)`;
  updateServoStateBadge();
  queueThrottledAngle(servoAngle);
}

function onSliderCommit(pctVal) {
  isDraggingSlider = false;
  lastSliderEditMs = Date.now();
  const parsedPct = parseInt(pctVal, 10);
  if (isNaN(parsedPct)) return;

  pendingServoCmd = null;
  liftPercent = Math.max(0, Math.min(100, parsedPct));
  servoAngle = percentToAngle(liftPercent, savedMinAngle, savedMaxAngle);

  const valEl = document.getElementById("s1-val");
  if (valEl) valEl.innerText = `${liftPercent}% (${servoAngle}°)`;
  sendCommand(`s1 ${servoAngle}`);
}

function nudgeLiftPercent(deltaPct) {
  lastSliderEditMs = Date.now();
  liftPercent = Math.max(0, Math.min(100, liftPercent + deltaPct));
  servoAngle = percentToAngle(liftPercent, savedMinAngle, savedMaxAngle);
  servoEnabled = true;
  updateServoUI();
  sendCommand(`s1 ${servoAngle}`);
}

function nudgeServo(deltaAngle) {
  lastSliderEditMs = Date.now();
  let next = servoAngle + deltaAngle;
  if (next < savedMinAngle) next = savedMinAngle;
  if (next > savedMaxAngle) next = savedMaxAngle;
  servoAngle = next;
  liftPercent = angleToPercent(servoAngle, savedMinAngle, savedMaxAngle);
  servoEnabled = true;
  updateServoUI();
  sendCommand(`s1 ${servoAngle}`);
}

function onMinTrimSlider(val) {
  const parsed = parseInt(val, 10);
  if (isNaN(parsed)) return;
  setMinTrimAngle(parsed, true);
}

function nudgeMinTrim(delta) {
  setMinTrimAngle(draftMinAngle + delta, false);
}

function setMinTrimAngle(target, throttled) {
  lastSliderEditMs = Date.now();
  let next = Math.max(0, Math.min(179, target));
  if (next >= draftMaxAngle) {
    next = draftMaxAngle - 1;
    showToast("Min End must stay below Max End", "warn");
  }
  draftMinAngle = next;
  servoAngle = next;
  liftPercent = angleToPercent(servoAngle, savedMinAngle, savedMaxAngle);
  servoEnabled = true;
  updateServoUI();

  if (throttled) {
    queueThrottledAngle(next);
  } else {
    pendingServoCmd = null;
    sendCommand(`s1 ${next}`);
  }
}

function onMaxTrimSlider(val) {
  const parsed = parseInt(val, 10);
  if (isNaN(parsed)) return;
  setMaxTrimAngle(parsed, true);
}

function nudgeMaxTrim(delta) {
  setMaxTrimAngle(draftMaxAngle + delta, false);
}

function setMaxTrimAngle(target, throttled) {
  lastSliderEditMs = Date.now();
  let next = Math.max(1, Math.min(180, target));
  if (next <= draftMinAngle) {
    next = draftMinAngle + 1;
    showToast("Max End must stay above Min End", "warn");
  }
  draftMaxAngle = next;
  servoAngle = next;
  liftPercent = angleToPercent(servoAngle, savedMinAngle, savedMaxAngle);
  servoEnabled = true;
  updateServoUI();

  if (throttled) {
    queueThrottledAngle(next);
  } else {
    pendingServoCmd = null;
    sendCommand(`s1 ${next}`);
  }
}

function testTrimEnd(which) {
  lastSliderEditMs = Date.now();
  const target = (which === "min") ? draftMinAngle : draftMaxAngle;
  servoAngle = target;
  liftPercent = angleToPercent(servoAngle, savedMinAngle, savedMaxAngle);
  servoEnabled = true;
  updateServoUI();
  sendCommand(`move ${target} 350`);
}

async function saveCalibrationOffset() {
  const btn = document.getElementById("save-cal-btn");
  const txt = document.getElementById("save-cal-text");
  if (txt) txt.innerText = "Saving...";
  if (btn) btn.style.background = "#0284c7";

  savedMinAngle = draftMinAngle;
  savedMaxAngle = draftMaxAngle;
  liftPercent = angleToPercent(servoAngle, savedMinAngle, savedMaxAngle);

  try {
    localStorage.setItem("mg996r_servo_range", JSON.stringify({ min: savedMinAngle, max: savedMaxAngle }));
  } catch (e) {}

  await sendCommand(`range_set ${savedMinAngle} ${savedMaxAngle}`);
  updateServoUI();

  appendLog("Calibration", `Saved Trim Range to NVS Flash: ${savedMinAngle}° – ${savedMaxAngle}°`);
  showToast(`Saved ${savedMinAngle}°–${savedMaxAngle}° to Flash! Slider updated.`, "success");

  if (txt) txt.innerText = "Saved to Flash";
  if (btn) btn.style.background = "#059669";
  setTimeout(() => {
    if (txt) txt.innerText = "Save Trim to Flash";
    if (btn) btn.style.background = "#10b981";
  }, 1500);
}

async function resetRangeTrim() {
  draftMinAngle = 0;
  draftMaxAngle = 180;
  savedMinAngle = 0;
  savedMaxAngle = 180;
  liftPercent = angleToPercent(servoAngle, savedMinAngle, savedMaxAngle);

  try {
    localStorage.setItem("mg996r_servo_range", JSON.stringify({ min: 0, max: 180 }));
  } catch (e) {}

  await sendCommand("range_set 0 180");
  updateServoUI();
  appendLog("Calibration", "Reset Trim Range to 0° – 180°");
  showToast("Trim range reset to 0°–180°", "info");
}

function moveToSavedMin() {
  servoAngle = savedMinAngle;
  liftPercent = 0;
  servoEnabled = true;
  updateServoUI();
  sendCommand("min");
  showToast(`Moving to Min (${savedMinAngle}°)`, "info");
}

function homeServo() {
  servoAngle = Math.round((savedMinAngle + savedMaxAngle) / 2);
  liftPercent = 50;
  servoEnabled = true;
  updateServoUI();
  sendCommand("home");
  showToast(`Centering to ${servoAngle}° (50%)`, "info");
}

function moveToSavedMax() {
  servoAngle = savedMaxAngle;
  liftPercent = 100;
  servoEnabled = true;
  updateServoUI();
  sendCommand("max");
  showToast(`Moving to Max (${savedMaxAngle}°)`, "info");
}

function detachServo() {
  servoEnabled = false;
  updateServoStateBadge();
  sendCommand("detach");
  showToast("Servo detached (Idle)", "warn");
}

function requestCalibrationSync() {
  sendCommand("cal_get");
  showToast("Fetching saved trim range...", "info");
}

function requestStatusSync() {
  sendCommand("status");
  showToast("Syncing servo state...", "info");
}

function applyReceivedAngle(angle, forceSync = false) {
  if (isNaN(angle)) return;
  const now = Date.now();
  if (!forceSync && (isDraggingSlider || (now - lastSliderEditMs) < 600)) {
    return;
  }
  servoAngle = Math.max(0, Math.min(180, angle));
  liftPercent = angleToPercent(servoAngle, savedMinAngle, savedMaxAngle);
  updateServoUI();
}

function applyReceivedRange(minA, maxA) {
  if (isNaN(minA) || isNaN(maxA)) return;
  const mn = Math.max(0, Math.min(179, minA));
  const mx = Math.max(mn + 1, Math.min(180, maxA));
  savedMinAngle = mn;
  savedMaxAngle = mx;
  draftMinAngle = mn;
  draftMaxAngle = mx;
  liftPercent = angleToPercent(servoAngle, savedMinAngle, savedMaxAngle);
  try {
    localStorage.setItem("mg996r_servo_range", JSON.stringify({ min: mn, max: mx }));
  } catch (e) {}
  updateServoUI();
}

function applyReceivedOffset(offset) {
  if (isNaN(offset)) return;
  // Kept for compatibility with CAL:<offset> telemetry
}

function applyReceivedEnableState(enabled) {
  servoEnabled = !!enabled;
  updateServoStateBadge();
}