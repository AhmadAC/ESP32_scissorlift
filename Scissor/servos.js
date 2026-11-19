let servoAngle = 90;
let servoOffset = 0;
let servoEnabled = false;
let autoIdleEnabled = false;
let joyInvertEnabled = false;
let servoName = "MG996R";
let isOffsetMode = false;

let isDraggingSlider = false;
let lastSliderEditMs = 0;
let pendingServoCmd = null;
let pendingCmdKind = null;
let servoThrottleTimer = null;

function initServoStorage() {
  try {
    const storedName = localStorage.getItem("mg996r_servo_name");
    if (storedName && storedName.trim().length > 0) {
      servoName = storedName.trim().substring(0, 28);
    }
    const storedOffset = localStorage.getItem("mg996r_servo_offset");
    if (storedOffset !== null) {
      const parsed = parseInt(storedOffset, 10);
      if (!isNaN(parsed)) {
        servoOffset = Math.max(-45, Math.min(45, parsed));
      }
    }
  } catch (e) {}
  applyAutoIdleState(autoIdleEnabled);
  applyJoystickInvertState(joyInvertEnabled);
  updateServoUI();
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

function updateServoUI() {
  const offEl = document.getElementById("o1-val");
  if (offEl) offEl.innerText = `${servoOffset > 0 ? "+" : ""}${servoOffset}°`;

  const sl = document.getElementById("s1-slider");
  const hdr = document.getElementById("s1-header");

  const btnA = document.getElementById("s1-btn-a");
  const btnB = document.getElementById("s1-btn-b");
  const btnC = document.getElementById("s1-btn-c");
  const btnD = document.getElementById("s1-btn-d");

  if (isOffsetMode) {
    if (sl) {
      sl.classList.add("offset-slider");
      sl.min = "-45";
      sl.max = "45";
      sl.value = servoOffset;
    }
    if (hdr) {
      hdr.innerHTML = `${escapeHtml(servoName)} Trim: <span id="s1-val" style="color:#f59e0b; font-family:monospace; font-weight:700;">${servoOffset > 0 ? "+" : ""}${servoOffset}°</span>`;
    }
    if (btnA) btnA.innerText = "-1 Trim";
    if (btnB) btnB.innerText = "+1 Trim";
    if (btnC) btnC.innerText = "-5°";
    if (btnD) btnD.innerText = "+5°";
  } else {
    if (sl) {
      sl.classList.remove("offset-slider");
      sl.min = "0";
      sl.max = "180";
      sl.value = servoAngle;
    }
    if (hdr) {
      hdr.innerHTML = `${escapeHtml(servoName)}: <span id="s1-val" style="color:#38bdf8; font-family:monospace; font-weight:700;">${servoAngle}°</span>`;
    }
    if (btnA) btnA.innerText = "-10°";
    if (btnB) btnB.innerText = "-1°";
    if (btnC) btnC.innerText = "+1°";
    if (btnD) btnD.innerText = "+10°";
  }
  updateServoStateBadge();
}

function editServoName() {
  const next = prompt("Rename Servo:", servoName || "MG996R");
  if (next && next.trim().length > 0) {
    servoName = next.trim().substring(0, 28);
    try { localStorage.setItem("mg996r_servo_name", servoName); } catch (e) {}
    updateServoUI();
  }
}

function toggleOffsetMode() {
  isOffsetMode = !isOffsetMode;
  const btn = document.getElementById("offset-mode-btn");
  if (btn) {
    if (isOffsetMode) {
      btn.innerText = "Trim: ON";
      btn.style.background = "#f59e0b";
      btn.style.color = "#000000";
      showToast("Calibration Trim Mode Active", "warn");
    } else {
      btn.innerText = "Trim: OFF";
      btn.style.background = "#1e293b";
      btn.style.color = "#f59e0b";
      showToast("Servo Angle Mode Active", "info");
    }
  }
  updateServoUI();
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

function onSliderStart() {
  isDraggingSlider = true;
  lastSliderEditMs = Date.now();
}

function onServoSlider(val) {
  const parsed = parseInt(val, 10);
  if (isNaN(parsed)) return;

  lastSliderEditMs = Date.now();
  servoEnabled = true;
  updateServoStateBadge();

  if (isOffsetMode) {
    servoOffset = Math.max(-45, Math.min(45, parsed));
    const offEl = document.getElementById("o1-val");
    if (offEl) offEl.innerText = `${servoOffset > 0 ? "+" : ""}${servoOffset}°`;
    const valEl = document.getElementById("s1-val");
    if (valEl) valEl.innerText = `${servoOffset > 0 ? "+" : ""}${servoOffset}°`;
    pendingCmdKind = "offset";
    pendingServoCmd = `offset ${servoOffset}`;
    try { localStorage.setItem("mg996r_servo_offset", String(servoOffset)); } catch (e) {}
  } else {
    servoAngle = Math.max(0, Math.min(180, parsed));
    const valEl = document.getElementById("s1-val");
    if (valEl) valEl.innerText = `${servoAngle}°`;
    pendingCmdKind = "angle";
    pendingServoCmd = `s1 ${servoAngle}`;
  }

  if (!servoThrottleTimer) {
    servoThrottleTimer = setTimeout(() => {
      servoThrottleTimer = null;
      const cmd = pendingServoCmd;
      pendingServoCmd = null;
      pendingCmdKind = null;
      if (cmd) sendCommand(cmd);
    }, 80);
  }
}

function onSliderCommit(val) {
  isDraggingSlider = false;
  lastSliderEditMs = Date.now();
  const parsed = parseInt(val, 10);
  if (isNaN(parsed)) return;

  pendingServoCmd = null;
  pendingCmdKind = null;

  if (isOffsetMode) {
    servoOffset = Math.max(-45, Math.min(45, parsed));
    sendCommand(`offset ${servoOffset}`);
    try { localStorage.setItem("mg996r_servo_offset", String(servoOffset)); } catch (e) {}
  } else {
    servoAngle = Math.max(0, Math.min(180, parsed));
    sendCommand(`s1 ${servoAngle}`);
  }
}

function servoStepAction(normalDelta, trimDelta) {
  if (isOffsetMode) {
    if (trimDelta === -1 || trimDelta === 1) {
      adjustOffset(trimDelta);
    } else {
      nudgeServo(trimDelta);
    }
  } else {
    nudgeServo(normalDelta);
  }
}

function nudgeServo(delta) {
  lastSliderEditMs = Date.now();
  let next = (servoAngle || 0) + delta;
  if (next < 0) next = 0;
  if (next > 180) next = 180;
  servoAngle = next;
  servoEnabled = true;
  updateServoStateBadge();

  if (!isOffsetMode) {
    const sl = document.getElementById("s1-slider");
    if (sl) sl.value = next;
    const valEl = document.getElementById("s1-val");
    if (valEl) valEl.innerText = `${next}°`;
  }
  sendCommand(`s1 ${next}`);
}

function adjustOffset(delta) {
  lastSliderEditMs = Date.now();
  let next = (servoOffset || 0) + delta;
  if (next < -45) next = -45;
  if (next > 45) next = 45;
  servoOffset = next;
  servoEnabled = true;
  updateServoStateBadge();

  const offEl = document.getElementById("o1-val");
  if (offEl) offEl.innerText = `${next > 0 ? "+" : ""}${next}°`;
  if (isOffsetMode) {
    const sl = document.getElementById("s1-slider");
    if (sl) sl.value = next;
    const valEl = document.getElementById("s1-val");
    if (valEl) valEl.innerText = `${next > 0 ? "+" : ""}${next}°`;
  }
  sendCommand(`offset ${next}`);
  appendLog("Trim", `Offset -> ${next}°`);
  try { localStorage.setItem("mg996r_servo_offset", String(servoOffset)); } catch (e) {}
}

async function saveCalibrationOffset() {
  const btn = document.getElementById("save-cal-btn");
  const txt = document.getElementById("save-cal-text");
  if (txt) txt.innerText = "Saving...";
  if (btn) btn.style.background = "#0284c7";

  try {
    localStorage.setItem("mg996r_servo_offset", String(servoOffset));
  } catch (e) {}

  await sendCommand(`cal_set ${servoOffset}`);
  if (isOffsetMode) toggleOffsetMode();
  else updateServoUI();

  appendLog("Calibration", `Saved Trim Offset to NVS: ${servoOffset}°`);
  showToast("Trim offset saved to ESP32 Flash!", "success");

  if (txt) txt.innerText = "Saved to Flash";
  if (btn) btn.style.background = "#059669";
  setTimeout(() => {
    if (txt) txt.innerText = "Save Trim to Flash";
    if (btn) btn.style.background = "#10b981";
  }, 1500);
}

function homeServo() {
  servoAngle = 90;
  servoEnabled = true;
  if (isOffsetMode) toggleOffsetMode();
  else updateServoUI();
  sendCommand("home");
  showToast("Centering MG996R to 90°", "info");
}

function detachServo() {
  servoEnabled = false;
  updateServoStateBadge();
  sendCommand("detach");
  showToast("Servo detached (Idle)", "warn");
}

function requestCalibrationSync() {
  sendCommand("cal_get");
  showToast("Fetching trim offset...", "info");
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
  if (!isOffsetMode) {
    const sl = document.getElementById("s1-slider");
    if (sl) sl.value = servoAngle;
    const lbl = document.getElementById("s1-val");
    if (lbl) lbl.innerText = `${servoAngle}°`;
  }
}

function applyReceivedOffset(offset) {
  if (isNaN(offset)) return;
  servoOffset = Math.max(-45, Math.min(45, offset));
  const offEl = document.getElementById("o1-val");
  if (offEl) offEl.innerText = `${servoOffset > 0 ? "+" : ""}${servoOffset}°`;
  if (isOffsetMode) updateServoUI();
}

function applyReceivedEnableState(enabled) {
  servoEnabled = !!enabled;
  updateServoStateBadge();
}