const PANELS = ["dataPanel", "configPanel", "wifiPanel"];
const DATA_REFRESH_MS = 500;
const WIFI_REFRESH_MS = 10000;
const TAB_ICONS = {
  dashboard: { active: "/icons/dashboard-active.svg", inactive: "/icons/dashboard.svg" },
  map: { active: "/icons/map-pinned-active.svg", inactive: "/icons/map-pinned.svg" },
  wifi: { active: "/icons/router-active.svg", inactive: "/icons/router.svg" },
};
const isProvisionMode = location.hostname === "192.168.4.1";

const fetchOpts = { cache: "no-store" };

const TAG_STRATEGIES = {
  1: { label: "统计", desc: "返回区域内运动人数与静止人数。" },
  2: { label: "边缘门", desc: "在检测区域边缘标定一条窄门线（短边约 10cm），穿越时触发进入/离开。" },
  3: { label: "靠近远离", desc: "靠近时红色指示灯亮起，远离时绿色指示灯亮起。" },
};

let areaConfig = { enabled: false, tags: [] };
let draftCoords = [[0, 0], [0, 0], [0, 0], [0, 0]];
let activePanel = "dataPanel";
let wifiFormDirty = false;
let wifiFieldsInitialized = false;

function showTab(panelId) {
  if (panelId === "configPanel" && !areaConfig.enabled) {
    return;
  }

  activePanel = panelId;
  document.querySelectorAll(".tab-button").forEach((btn) => {
    const target = btn.dataset.panel;
    if (!target || btn.classList.contains("tab-hidden")) {
      return;
    }
    const active = target === panelId;
    btn.classList.toggle("tab-active", active);

    const iconKey = btn.dataset.icon;
    const icon = btn.querySelector("img");
    if (icon && iconKey && TAB_ICONS[iconKey]) {
      icon.src = active ? TAB_ICONS[iconKey].active : TAB_ICONS[iconKey].inactive;
    }
  });

  PANELS.forEach((id) => {
    document.getElementById(id).classList.toggle("panel-hidden", id !== panelId);
  });
}

function setMessage(text, type) {
  const msg = document.getElementById("msg");
  if (!msg) {
    return;
  }
  msg.innerHTML = text;
  msg.className = type ? `message-box ${type}` : "message-box";
}

function updateAreaVisibility() {
  const tab = document.getElementById("areaConfigTab");
  const activityPanel = document.getElementById("activityPanel");
  const setupPrompt = document.getElementById("areaSetupPrompt");

  if (areaConfig.enabled) {
    tab.classList.remove("tab-hidden");
  } else {
    tab.classList.add("tab-hidden");
    if (activePanel === "configPanel") {
      showTab("dataPanel");
    }
  }

  if (!areaConfig.enabled) {
    activityPanel.classList.add("panel-hidden");
    setupPrompt.classList.remove("panel-hidden");
    return;
  }

  setupPrompt.classList.add("panel-hidden");
  if (areaConfig.tags.length > 0) {
    activityPanel.classList.remove("panel-hidden");
  } else {
    activityPanel.classList.add("panel-hidden");
  }
}

function strategyLabel(value) {
  return TAG_STRATEGIES[value]?.label || `策略${value}`;
}

function updateStrategyDesc() {
  const select = document.getElementById("tagStrategy");
  const desc = document.getElementById("tagStrategyDesc");
  if (!select || !desc) {
    return;
  }
  const info = TAG_STRATEGIES[Number(select.value)];
  desc.textContent = info ? info.desc : "";
}

function renderTagList() {
  const list = document.getElementById("tagList");
  const empty = document.getElementById("tagEmptyState");
  list.innerHTML = "";

  if (areaConfig.tags.length === 0) {
    empty.classList.remove("panel-hidden");
    return;
  }

  empty.classList.add("panel-hidden");
  areaConfig.tags.forEach((tag) => {
    const item = document.createElement("div");
    item.className = "tag-list-item";
    item.innerHTML = `
      <div class="tag-list-info">
        <strong>${tag.name}</strong>
        <span>${strategyLabel(tag.strategy)}</span>
      </div>
      <button type="button" class="tag-delete-btn" data-id="${tag.id}">删除</button>
    `;
    list.appendChild(item);
  });

  list.querySelectorAll(".tag-delete-btn").forEach((btn) => {
    btn.addEventListener("click", () => deleteTag(Number(btn.dataset.id)));
  });
}

function renderSignalLamp(label, color, on) {
  const stateText = on ? "触发" : "未触发";
  return `
    <div class="signal-card signal-card--${color}">
      <div class="signal-label">${label}</div>
      <div class="signal-lamp-wrap">
        <span class="signal-lamp signal-lamp--${color}${on ? " signal-lamp--on" : ""}"></span>
      </div>
      <div class="signal-state${on ? " signal-state--active" : ""}">${stateText}</div>
    </div>
  `;
}

function buildAreaMetrics(area) {
  const strategy = Number(area.strategy) || 1;

  if (strategy === 2) {
    return `
      <div class="area-metrics area-metrics--signal">
        ${renderSignalLamp("进入", "red", !!area.enter)}
        ${renderSignalLamp("离开", "green", !!area.exit)}
      </div>
    `;
  }

  if (strategy === 3) {
    return `
      <div class="area-metrics area-metrics--signal">
        ${renderSignalLamp("靠近", "red", !!area.approaching)}
        ${renderSignalLamp("远离", "green", !!area.leaving)}
      </div>
    `;
  }

  return `
    <div class="area-metrics">
      <div class="status-card">
        <div class="status-label">
          <span class="status-dot"></span>
          <span class="status-text">活动人数</span>
        </div>
        <div class="status-value">${area.active ?? 0}人</div>
      </div>
      <div class="status-card">
        <div class="status-label">
          <span class="status-dot"></span>
          <span class="status-text">静止人数</span>
        </div>
        <div class="status-value">${area.static ?? 0}人</div>
      </div>
    </div>
  `;
}

function renderAreaCards(areas) {
  const list = document.getElementById("areaList");
  if (!list) {
    return;
  }
  list.innerHTML = "";

  areas.forEach((area, index) => {
    const card = document.createElement("div");
    card.className = "gjs-t-border area-card";
    card.innerHTML = `
      <div class="area-header">
        <div class="area-title-group">
          <div class="area-marker"></div>
          <div class="area-title">${area.name}</div>
        </div>
        <div class="area-status-badge">${strategyLabel(area.strategy)} · 区域${index + 1}</div>
      </div>
      ${buildAreaMetrics(area)}
    `;
    list.appendChild(card);
  });
}

function resetDraftCoords() {
  draftCoords = [[0, 0], [0, 0], [0, 0], [0, 0]];
  updateCoordSummary();
}

function updateCoordSummary() {
  const el = document.getElementById("coordSummary");
  if (!el) {
    return;
  }
  const parts = draftCoords.map((c, i) => `P${i + 1}(${c[0]},${c[1]})`);
  const hasAny = draftCoords.some((c) => c[0] !== 0 || c[1] !== 0);
  el.textContent = hasAny ? parts.join(" · ") : "尚未获取坐标";
}

function openTagModal() {
  document.getElementById("tagName").value = "";
  document.getElementById("tagStrategy").value = "1";
  updateStrategyDesc();
  resetDraftCoords();
  const modal = document.getElementById("tagModal");
  modal.classList.remove("panel-hidden");
  modal.setAttribute("aria-hidden", "false");
}

function closeTagModal() {
  const modal = document.getElementById("tagModal");
  modal.classList.add("panel-hidden");
  modal.setAttribute("aria-hidden", "true");
}

function shouldSyncWifiFields() {
  if (isProvisionMode || wifiFormDirty) {
    return false;
  }

  const ssidEl = document.getElementById("ssid");
  const passEl = document.getElementById("password");
  if (document.activeElement === ssidEl || document.activeElement === passEl) {
    return false;
  }

  return !wifiFieldsInitialized;
}

async function loadWifiStatus() {
  try {
    const res = await fetch("/api/wifi", fetchOpts);
    const wifi = await res.json();

    if (shouldSyncWifiFields()) {
      document.getElementById("ssid").value = wifi.ssid || "";
      document.getElementById("password").value = wifi.password || "";
      wifiFieldsInitialized = true;
    }

    const connStatus = document.getElementById("connStatus");
    const connDesc = document.getElementById("connDesc");
    if (!connStatus || !connDesc) {
      return;
    }

    if (wifi.connected) {
      connStatus.textContent = "已连接";
      connDesc.textContent = wifi.ssid
        ? `已接入 ${wifi.ssid}${wifi.ip ? `，IP：${wifi.ip}` : ""}`
        : `已接入局域网${wifi.ip ? `，IP：${wifi.ip}` : ""}`;
    } else if (wifi.ap_mode || isProvisionMode) {
      connStatus.textContent = "待配置";
      connDesc.textContent = "完成 WiFi 保存后，设备将尝试建立网络接入。";
    } else if (wifi.configured) {
      connStatus.textContent = "连接中";
      connDesc.textContent = `正在连接 ${wifi.ssid}…`;
    } else {
      connStatus.textContent = "待配置";
      connDesc.textContent = "完成 WiFi 保存后，设备将尝试建立网络接入。";
    }

    const reprovisionBlock = document.getElementById("reprovisionBlock");
    if (reprovisionBlock) {
      reprovisionBlock.classList.toggle("panel-hidden", isProvisionMode || !wifi.configured);
    }
  } catch (err) {
    console.error("loadWifiStatus failed:", err);
  }
}

async function loadAreaConfig() {
  try {
    const res = await fetch("/api/config", fetchOpts);
    areaConfig = await res.json();
    if (!Array.isArray(areaConfig.tags)) {
      areaConfig.tags = [];
    }
    updateAreaVisibility();
    renderTagList();
  } catch (err) {
    console.error("loadAreaConfig failed:", err);
  }
}

async function refresh() {
  try {
    const res = await fetch("/api/data", fetchOpts);
    const data = await res.json();

    const tempEl = document.getElementById("temp");
    const co2El = document.getElementById("co2");
    if (data.env_ready) {
      tempEl.textContent = `${data.temp} / ${data.humidity}`;
      co2El.textContent = `${data.co2} ppm`;
    } else {
      tempEl.textContent = "-- / --";
      co2El.textContent = "预热中…";
    }

    if (areaConfig.enabled && Array.isArray(data.areas) && data.areas.length > 0) {
      renderAreaCards(data.areas);
    }
  } catch (err) {
    console.error("refresh failed:", err);
  }
}

function startDataPolling() {
  const tick = async () => {
    await refresh();
    setTimeout(tick, DATA_REFRESH_MS);
  };
  tick();
}

async function postConfig(body) {
  const res = await fetch("/api/config", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  return res.json();
}

async function enableAreaConfig() {
  try {
    const data = await postConfig({ action: "enable", enabled: true });
    if (data.ok) {
      await loadAreaConfig();
      showTab("configPanel");
    } else {
      alert(data.message || "启用失败");
    }
  } catch (err) {
    alert("网络错误");
  }
}

async function fetchCoord(point) {
  const btn = document.querySelector(`.coord-btn[data-point="${point}"]`);
  if (btn) {
    btn.disabled = true;
    btn.textContent = `◎ 读取中…`;
  }
  try {
    const res = await fetch(`/api/coord?point=${point}`, fetchOpts);
    const data = await res.json();
    if (data.ok) {
      draftCoords[point - 1] = [data.x, data.y];
      updateCoordSummary();
    } else {
      alert(data.message || "获取坐标失败");
    }
  } catch (err) {
    console.error("fetchCoord failed:", err);
    alert("网络错误，获取坐标失败");
  } finally {
    if (btn) {
      btn.disabled = false;
      btn.textContent = `◎ 获取坐标${point}`;
    }
  }
}

function coordsReady() {
  const xs = draftCoords.map((c) => c[0]);
  const ys = draftCoords.map((c) => c[1]);
  const minX = Math.min(...xs);
  const maxX = Math.max(...xs);
  const minY = Math.min(...ys);
  const maxY = Math.max(...ys);
  const w = maxX - minX;
  const h = maxY - minY;
  return w >= 10 && h >= 10;
}

async function confirmAddTag() {
  const name = document.getElementById("tagName").value.trim();
  const strategy = Number(document.getElementById("tagStrategy").value);
  if (!name) {
    alert("请输入标签名字");
    return;
  }
  if (!coordsReady()) {
    alert("请先获取 4 个角点坐标，且区域宽高至少 10cm");
    return;
  }

  try {
    const data = await postConfig({
      action: "add_tag",
      strategy,
      name,
      coords: draftCoords,
    });
    if (data.ok) {
      closeTagModal();
      await loadAreaConfig();
      await refresh();
    } else {
      alert(data.message || "添加失败");
    }
  } catch (err) {
    alert("网络错误");
  }
}

async function deleteTag(id) {
  if (!confirm("确定删除该区域标签？")) {
    return;
  }
  try {
    const data = await postConfig({ action: "delete_tag", id });
    if (data.ok) {
      await loadAreaConfig();
      await refresh();
    } else {
      alert(data.message || "删除失败");
    }
  } catch (err) {
    alert("网络错误");
  }
}

function setupWifiForm() {
  const desc = document.getElementById("wifiDesc");
  const tip = document.getElementById("wifiTip");
  const ssidEl = document.getElementById("ssid");
  const passEl = document.getElementById("password");

  const markWifiFormDirty = () => {
    wifiFormDirty = true;
  };
  ssidEl.addEventListener("input", markWifiFormDirty);
  passEl.addEventListener("input", markWifiFormDirty);

  if (isProvisionMode) {
    desc.innerHTML =
      "请连接热点 <b>MCP-Sensors-Setup</b> 后，填写要接入的路由器 WiFi 信息。";
    tip.innerHTML =
      "连接热点后应自动弹出配置页；若未弹出，请手动打开 <b>http://192.168.4.1</b>。配置成功后请在同一局域网访问 <b>http://mcp-sensors.local</b>。";
  } else {
    tip.textContent =
      "修改 WiFi 后设备将重新连接路由器，请在同一局域网通过 http://mcp-sensors.local 访问。";
  }

  document.getElementById("reprovisionBtn")?.addEventListener("click", async () => {
    if (!confirm("将开启配网热点 MCP-Sensors-Setup，当前 WiFi 连接会断开。是否继续？")) {
      return;
    }
    setMessage("正在开启配网热点…", "");
    try {
      const res = await fetch("/api/wifi", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ action: "reprovision" }),
      });
      const data = await res.json();
      if (data.ok) {
        setMessage(
          "热点已开启，请连接 <b>MCP-Sensors-Setup</b>，浏览器打开 <b>http://192.168.4.1</b> 配置新 WiFi。",
          "ok"
        );
      } else {
        setMessage(data.message || "开启失败", "err");
      }
    } catch (err) {
      setMessage("网络错误，请重试", "err");
    }
  });

  document.getElementById("wifiForm").addEventListener("submit", async (e) => {
    e.preventDefault();

    const btn = document.getElementById("submitBtn");
    const ssid = document.getElementById("ssid").value.trim();
    btn.disabled = true;

    if (!ssid) {
      setMessage("请输入 WiFi 名称后再提交。", "err");
      btn.disabled = false;
      return;
    }

    setMessage("正在提交 WiFi 配置…", "");

    try {
      const res = await fetch("/api/wifi", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          ssid,
          password: document.getElementById("password").value,
        }),
      });
      const data = await res.json();

      if (data.ok) {
        wifiFormDirty = false;
        wifiFieldsInitialized = true;
        setMessage(
          isProvisionMode
            ? "正在连接路由器…<br>请切换回家庭 WiFi，然后在浏览器打开 <b>http://mcp-sensors.local</b>"
            : "正在连接 WiFi，请稍候…",
          "ok"
        );
      } else {
        setMessage(data.message || "配置失败", "err");
        btn.disabled = false;
      }
    } catch (err) {
      setMessage("网络错误，请重试", "err");
      btn.disabled = false;
    }
  });
}

function setupAreaConfig() {
  document.getElementById("enableAreaBtn").addEventListener("click", enableAreaConfig);
  document.getElementById("addTagBtn").addEventListener("click", openTagModal);
  document.getElementById("closeTagModal").addEventListener("click", closeTagModal);
  document.getElementById("cancelAddTag").addEventListener("click", closeTagModal);
  document.getElementById("confirmAddTag").addEventListener("click", confirmAddTag);
  document.getElementById("tagStrategy").addEventListener("change", updateStrategyDesc);
  document.getElementById("tagModal").addEventListener("click", (e) => {
    if (e.target.id === "tagModal") {
      closeTagModal();
    }
  });
  document.querySelectorAll(".coord-btn").forEach((btn) => {
    btn.addEventListener("click", () => fetchCoord(Number(btn.dataset.point)));
  });
}

document.querySelectorAll(".tab-button").forEach((btn) => {
  btn.addEventListener("click", () => {
    if (btn.dataset.panel) {
      showTab(btn.dataset.panel);
    }
  });
});

setupWifiForm();
setupAreaConfig();

loadAreaConfig().then(() => {
  loadWifiStatus();
  startDataPolling();
  setInterval(loadWifiStatus, WIFI_REFRESH_MS);
  showTab(isProvisionMode ? "wifiPanel" : "dataPanel");
});
