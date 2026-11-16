function handleSendCustom(e) {
  e.preventDefault();
  const input = document.getElementById("cmd-input");
  const val = input.value.trim();
  if (!val) return;
  sendCommand(val);
  input.value = "";
}

window.addEventListener("DOMContentLoaded", () => {
  initBackendUI();
  initServoStorage();
  initActionsStorage();

  // Keyboard navigation shortcuts for precision servo testing
  window.addEventListener("keydown", (e) => {
    // Ignore keystrokes when typing into text inputs
    if (e.target.tagName === "INPUT" || e.target.tagName === "TEXTAREA") return;

    if (e.key === "ArrowLeft") {
      nudgeServo(-1);
      e.preventDefault();
    } else if (e.key === "ArrowRight") {
      nudgeServo(1);
      e.preventDefault();
    } else if (e.key === "ArrowDown") {
      nudgeServo(-10);
      e.preventDefault();
    } else if (e.key === "ArrowUp") {
      nudgeServo(10);
      e.preventDefault();
    } else if (e.key === " " || e.key === "Spacebar") {
      homeServo();
      e.preventDefault();
    } else if (e.key === "Escape") {
      stopAllMotion();
      e.preventDefault();
    }
  });
});