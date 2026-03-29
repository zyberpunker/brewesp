function setButtonState(button, baseClasses, stateClass) {
  if (!button) {
    return;
  }
  button.className = `${baseClasses} ${stateClass}`.trim();
}

function stateFromAction(action) {
  switch (action) {
    case "heating_on":
      return { heating_state: "on" };
    case "heating_off":
      return { heating_state: "off" };
    case "cooling_on":
      return { cooling_state: "on" };
    case "cooling_off":
      return { cooling_state: "off" };
    case "all_off":
      return { heating_state: "off", cooling_state: "off" };
    default:
      return {};
  }
}

function withDerivedButtonStates(payload) {
  const next = { ...payload };

  if (next.heating_state) {
    next.heating_on_button_state = next.heating_state === "on" ? "active" : "inactive";
    next.heating_off_button_state = next.heating_state === "off" ? "active" : "inactive";
  }

  if (next.cooling_state) {
    next.cooling_on_button_state = next.cooling_state === "on" ? "active" : "inactive";
    next.cooling_off_button_state = next.cooling_state === "off" ? "active" : "inactive";
  }

  return next;
}

function formatMetric(value, format) {
  if (value === null || value === undefined || Number.isNaN(value)) {
    return "n/a";
  }
  if (format === "temperature") {
    return `${Number(value).toFixed(1)}°C`;
  }
  return String(value);
}

function formatTimestamp(value) {
  if (!value) {
    return "No timestamp";
  }

  const date = new Date(value);
  if (Number.isNaN(date.getTime())) {
    return value;
  }

  return new Intl.DateTimeFormat(undefined, {
    month: "short",
    day: "numeric",
    hour: "2-digit",
    minute: "2-digit",
  }).format(date);
}

function buildPath(points, xPositions, minValue, maxValue, width, height, padding) {
  const innerHeight = height - padding.top - padding.bottom;
  const range = maxValue - minValue || 1;
  let path = "";
  let drawing = false;

  points.forEach((value, index) => {
    if (typeof value !== "number") {
      drawing = false;
      return;
    }

    const x = xPositions[index];
    const y = height - padding.bottom - ((value - minValue) / range) * innerHeight;
    path += `${drawing ? " L" : "M"} ${x.toFixed(2)} ${y.toFixed(2)}`;
    drawing = true;
  });

  return path;
}

function buildTooltip() {
  const tooltip = document.createElement("div");
  tooltip.className = "chart-tooltip";
  tooltip.innerHTML = '<div class="chart-tooltip__time"></div><div class="chart-tooltip__rows"></div>';
  return tooltip;
}

function renderLineChart(container) {
  const points = JSON.parse(container.dataset.chartPoints || "[]");
  const series = JSON.parse(container.dataset.chartSeries || "[]");
  const width = container.clientWidth || 320;
  const height = container.classList.contains("line-chart--large") ? 320 : 220;
  const padding = { top: 18, right: 18, bottom: 28, left: 18 };
  const numericValues = [];

  series.forEach((definition) => {
    points.forEach((point) => {
      const value = point[definition.field];
      if (typeof value === "number") {
        numericValues.push(value);
      }
    });
  });

  container.innerHTML = "";

  if (numericValues.length < 2 || points.length < 2) {
    const empty = document.createElement("div");
    empty.className = "chart-empty";
    empty.textContent = "No temperature telemetry yet";
    container.appendChild(empty);
    return;
  }

  const minValue = Math.min(...numericValues);
  const maxValue = Math.max(...numericValues);
  const innerWidth = width - padding.left - padding.right;
  const xPositions = points.map((_, index) => {
    if (points.length === 1) {
      return padding.left + innerWidth / 2;
    }
    return padding.left + (innerWidth / (points.length - 1)) * index;
  });

  const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  svg.setAttribute("viewBox", `0 0 ${width} ${height}`);
  svg.setAttribute("preserveAspectRatio", "none");

  for (let i = 0; i < 4; i += 1) {
    const y = padding.top + ((height - padding.top - padding.bottom) / 3) * i;
    const line = document.createElementNS("http://www.w3.org/2000/svg", "line");
    line.setAttribute("x1", String(padding.left));
    line.setAttribute("x2", String(width - padding.right));
    line.setAttribute("y1", y.toFixed(2));
    line.setAttribute("y2", y.toFixed(2));
    line.setAttribute("stroke", "rgba(73,69,79,0.16)");
    line.setAttribute("stroke-width", "1");
    svg.appendChild(line);
  }

  const markerLayer = document.createElementNS("http://www.w3.org/2000/svg", "g");
  const markerLine = document.createElementNS("http://www.w3.org/2000/svg", "line");
  markerLine.setAttribute("y1", String(padding.top));
  markerLine.setAttribute("y2", String(height - padding.bottom));
  markerLine.setAttribute("stroke", "rgba(73,69,79,0.38)");
  markerLine.setAttribute("stroke-dasharray", "4 4");
  markerLine.setAttribute("opacity", "0");
  markerLayer.appendChild(markerLine);

  const markerDots = series.map((definition) => {
    const dot = document.createElementNS("http://www.w3.org/2000/svg", "circle");
    dot.setAttribute("r", "4.5");
    dot.setAttribute("fill", definition.color);
    dot.setAttribute("stroke", "#fff");
    dot.setAttribute("stroke-width", "2");
    dot.setAttribute("opacity", "0");
    markerLayer.appendChild(dot);
    return dot;
  });

  series.forEach((definition) => {
    const values = points.map((point) => point[definition.field]);
    const path = document.createElementNS("http://www.w3.org/2000/svg", "path");
    path.setAttribute("d", buildPath(values, xPositions, minValue, maxValue, width, height, padding));
    path.setAttribute("fill", "none");
    path.setAttribute("stroke", definition.color);
    path.setAttribute("stroke-width", definition.field === "setpoint_c" ? "2" : "3");
    path.setAttribute("stroke-linecap", "round");
    path.setAttribute("stroke-linejoin", "round");
    if (definition.field === "setpoint_c") {
      path.setAttribute("stroke-dasharray", "8 6");
    }
    svg.appendChild(path);
  });

  svg.appendChild(markerLayer);
  container.appendChild(svg);

  const tooltip = buildTooltip();
  container.appendChild(tooltip);

  const overlay = document.createElement("div");
  overlay.style.position = "absolute";
  overlay.style.inset = "0";
  overlay.style.cursor = "crosshair";
  container.appendChild(overlay);

  const updateHover = (event) => {
    const rect = container.getBoundingClientRect();
    const x = Math.min(Math.max(event.clientX - rect.left, padding.left), width - padding.right);
    const ratio = (x - padding.left) / Math.max(1, innerWidth);
    const index = Math.min(points.length - 1, Math.max(0, Math.round(ratio * (points.length - 1))));
    const point = points[index];

    markerLine.setAttribute("x1", xPositions[index].toFixed(2));
    markerLine.setAttribute("x2", xPositions[index].toFixed(2));
    markerLine.setAttribute("opacity", "1");

    const rows = [];
    series.forEach((definition, seriesIndex) => {
      const value = point[definition.field];
      if (typeof value !== "number") {
        markerDots[seriesIndex].setAttribute("opacity", "0");
        return;
      }

      const y = height - padding.bottom - ((value - minValue) / (maxValue - minValue || 1)) * (height - padding.top - padding.bottom);
      markerDots[seriesIndex].setAttribute("cx", xPositions[index].toFixed(2));
      markerDots[seriesIndex].setAttribute("cy", y.toFixed(2));
      markerDots[seriesIndex].setAttribute("opacity", "1");
      rows.push(
        `<div class="chart-tooltip__row"><span><span class="chart-tooltip__dot" style="background:${definition.color}"></span>${definition.label}</span><strong>${formatMetric(value, definition.format)}</strong></div>`,
      );
    });

    tooltip.querySelector(".chart-tooltip__time").textContent = formatTimestamp(point.recorded_at);
    tooltip.querySelector(".chart-tooltip__rows").innerHTML = rows.join("");

    const tooltipX = Math.min(rect.width - 180, Math.max(8, event.clientX - rect.left + 14));
    const tooltipY = Math.max(8, event.clientY - rect.top - 18);
    tooltip.style.left = `${tooltipX}px`;
    tooltip.style.top = `${tooltipY}px`;
    tooltip.classList.add("visible");
  };

  const clearHover = () => {
    tooltip.classList.remove("visible");
    markerLine.setAttribute("opacity", "0");
    markerDots.forEach((dot) => dot.setAttribute("opacity", "0"));
  };

  overlay.addEventListener("mousemove", updateHover);
  overlay.addEventListener("mouseenter", updateHover);
  overlay.addEventListener("mouseleave", clearHover);
}

function bootCharts() {
  document.querySelectorAll(".line-chart[data-chart-points]").forEach((container) => {
    renderLineChart(container);
  });
}

function applyLiveState(payload) {
  const status = document.querySelector("#device-status");
  const mqtt = document.querySelector("#mqtt-state");
  const lastHeartbeat = document.querySelector("#last-heartbeat");
  const modeChip = document.querySelector("#mode-chip");
  const controllerState = document.querySelector("#controller-state");
  const controllerReason = document.querySelector("#controller-reason");
  const beerProbeStatus = document.querySelector("#beer-probe-status");
  const beerProbeRom = document.querySelector("#beer-probe-rom");
  const chamberProbeStatus = document.querySelector("#chamber-probe-status");
  const chamberProbeRom = document.querySelector("#chamber-probe-rom");
  const secondaryEnabled = document.querySelector("#secondary-enabled");
  const controlSensor = document.querySelector("#control-sensor");
  const heatingState = document.querySelector("#heating-state");
  const coolingState = document.querySelector("#cooling-state");
  const heatingStatusText = document.querySelector("#heating-status-text");
  const coolingStatusText = document.querySelector("#cooling-status-text");
  const heatingStatusCard = document.querySelector("#heating-status-card");
  const coolingStatusCard = document.querySelector("#cooling-status-card");
  const tempPrimary = document.querySelector("#temp-primary");
  const tempTarget = document.querySelector("#temp-target");
  const configDesiredVersion = document.querySelector("#config-desired-version");
  const configLastApply = document.querySelector("#config-last-apply");
  const configMessage = document.querySelector("#config-message");
  const desiredVersionInput = document.querySelector("#desired-version-input");

  if (status) {
    status.textContent = payload.display_status;
  }
  if (mqtt) {
    mqtt.textContent = payload.mqtt_connected ? "connected" : "disconnected";
  }
  if (lastHeartbeat) {
    lastHeartbeat.textContent = payload.last_heartbeat_label || payload.last_heartbeat_at || "never";
  }
  if (modeChip) {
    modeChip.textContent = payload.last_mode || "mode n/a";
  }
  if (controllerState) {
    controllerState.textContent = payload.controller_state || "unknown";
  }
  if (controllerReason) {
    controllerReason.textContent = payload.controller_reason || "n/a";
  }
  if (beerProbeStatus) {
    beerProbeStatus.textContent = payload.beer_probe_present
      ? (payload.beer_probe_valid ? "present" : "invalid")
      : "missing";
  }
  if (beerProbeRom) {
    beerProbeRom.textContent = payload.beer_probe_rom || "n/a";
  }
  if (chamberProbeStatus) {
    chamberProbeStatus.textContent = payload.chamber_probe_present
      ? (payload.chamber_probe_valid ? "present" : "invalid")
      : "missing";
  }
  if (chamberProbeRom) {
    chamberProbeRom.textContent = payload.chamber_probe_rom || "n/a";
  }
  if (secondaryEnabled) {
    secondaryEnabled.textContent = payload.secondary_sensor_enabled ? "yes" : "no";
  }
  if (controlSensor) {
    controlSensor.textContent = payload.control_sensor || "primary";
  }
  if (heatingState) {
    heatingState.textContent = payload.heating_state;
  }
  if (coolingState) {
    coolingState.textContent = payload.cooling_state;
  }
  if (heatingStatusText) {
    heatingStatusText.textContent = payload.heating_state;
  }
  if (coolingStatusText) {
    coolingStatusText.textContent = payload.cooling_state;
  }
  if (tempPrimary) {
    tempPrimary.textContent = formatMetric(payload.last_temp_c, "temperature");
  }
  if (tempTarget) {
    tempTarget.textContent = formatMetric(payload.last_target_temp_c, "temperature");
  }
  if (configDesiredVersion) {
    const desiredVersion = payload.fermentation_config?.desired_version;
    configDesiredVersion.textContent = `Desired version ${desiredVersion ?? 1}`;
  }
  if (configLastApply) {
    const appliedResult = payload.fermentation_config?.last_applied_result;
    const appliedVersion = payload.fermentation_config?.last_applied_version;
    configLastApply.textContent = appliedResult
      ? `Last apply: ${appliedResult}${appliedVersion ? ` v${appliedVersion}` : ""}`
      : "Last apply: pending";
  }
  if (configMessage) {
    const message = payload.fermentation_config?.last_applied_message;
    configMessage.textContent = message || "";
    configMessage.hidden = !message;
  }
  if (desiredVersionInput) {
    const desiredVersion = payload.fermentation_config?.desired_version;
    desiredVersionInput.value = String((desiredVersion ?? 0) + 1);
  }
  if (heatingStatusCard) {
    heatingStatusCard.className = `control-status-card ${payload.heating_on_button_state}`;
  }
  if (coolingStatusCard) {
    coolingStatusCard.className = `control-status-card ${payload.cooling_on_button_state}`;
  }

  setButtonState(
    document.querySelector("#heating-on-button"),
    "toggle-button heat",
    payload.heating_on_button_state,
  );
  setButtonState(
    document.querySelector("#heating-off-button"),
    "toggle-button heat-off",
    payload.heating_off_button_state,
  );
  setButtonState(
    document.querySelector("#cooling-on-button"),
    "toggle-button cool",
    payload.cooling_on_button_state,
  );
  setButtonState(
    document.querySelector("#cooling-off-button"),
    "toggle-button cool-off",
    payload.cooling_off_button_state,
  );
}

async function fetchLiveState(url) {
  const response = await fetch(url, { headers: { Accept: "application/json" } });
  if (!response.ok) {
    throw new Error(`Live state failed: ${response.status}`);
  }
  return response.json();
}

function bootDeviceLive() {
  const liveRoot = document.querySelector("[data-device-live-url]");
  if (!liveRoot) {
    return;
  }

  const liveUrl = liveRoot.dataset.deviceLiveUrl;
  if (!liveUrl) {
    return;
  }

  let pollTimer = null;

  const poll = async () => {
    try {
      const payload = await fetchLiveState(liveUrl);
      applyLiveState(payload);
    } catch (_error) {
      // Keep polling; transient lag is acceptable.
    }
  };

  document.querySelectorAll("form[data-async-form='command']").forEach((form) => {
    const postUrl = form.getAttribute("action");

    form.querySelectorAll("button[name='action']").forEach((button) => {
      button.addEventListener("click", () => {
        form.dataset.lastAction = button.value;
      });
    });

    form.addEventListener("submit", async (event) => {
      event.preventDefault();
      const submitter = event.submitter;
      const formData = new FormData(form);
      const action = submitter?.value || form.dataset.lastAction || formData.get("action");
      if (submitter?.name && submitter?.value) {
        formData.set(submitter.name, submitter.value);
      } else if (action) {
        formData.set("action", action);
      }

      const buttons = form.querySelectorAll("button");
      buttons.forEach((button) => {
        button.disabled = true;
      });

      if (action) {
        applyLiveState(withDerivedButtonStates(stateFromAction(action)));
      }

      try {
        await fetch(postUrl, {
          method: "POST",
          body: formData,
          headers: { Accept: "text/html" },
        });
      } finally {
        buttons.forEach((button) => {
          button.disabled = false;
        });
      }

      poll();
      window.setTimeout(poll, 350);
      window.setTimeout(poll, 1200);
    });
  });

  document.querySelectorAll("form[data-async-form='fermentation']").forEach((form) => {
    const postUrl = form.getAttribute("action");
    const submitButton = form.querySelector("button[type='submit']");

    form.addEventListener("submit", async (event) => {
      event.preventDefault();

      const formData = new FormData(form);
      const setpoint = Number(formData.get("setpoint_c"));
      const mode = `${formData.get("mode") || "thermostat"}`;
      const pendingVersion = Number(formData.get("desired_version"));
      const configLastApply = document.querySelector("#config-last-apply");
      const configMessage = document.querySelector("#config-message");
      const tempTarget = document.querySelector("#temp-target");
      const modeChip = document.querySelector("#mode-chip");

      if (submitButton) {
        submitButton.disabled = true;
      }

      if (tempTarget && Number.isFinite(setpoint)) {
        tempTarget.textContent = formatMetric(setpoint, "temperature");
      }
      if (modeChip) {
        modeChip.textContent = mode;
      }
      if (configLastApply) {
        configLastApply.textContent = `Last apply: pending v${pendingVersion}`;
      }
      if (configMessage) {
        configMessage.hidden = true;
        configMessage.textContent = "";
      }

      try {
        await fetch(postUrl, {
          method: "POST",
          body: formData,
          headers: { Accept: "text/html" },
        });
      } finally {
        if (submitButton) {
          submitButton.disabled = false;
        }
      }

      poll();
      window.setTimeout(poll, 350);
      window.setTimeout(poll, 1200);
      window.setTimeout(poll, 2500);
    });
  });

  poll();
  pollTimer = window.setInterval(poll, 2000);

  window.addEventListener(
    "beforeunload",
    () => {
      if (pollTimer) {
        window.clearInterval(pollTimer);
      }
    },
    { once: true },
  );
}

window.addEventListener("load", () => {
  bootCharts();
  bootDeviceLive();
});

window.addEventListener("resize", bootCharts);
