import { COUNTERS, STATE_NAMES, ProtocolError, validateBackup, parseJournalDump,
  counterDelta, diagnosticHints, RECONNECT_OUTCOMES, INPUT_MODES, MAPPING_SOURCES, MAPPING_TARGETS,
  defaultInputMapping, encodeInputMapping } from "./protocol.mjs?v=19.0";
import { ReceiverManager } from "./manager.mjs?v=19.0";
import { initialLanguage, setLanguage, getLanguage, locale, localize } from "./i18n.mjs?v=19.0";

let languageStorage;
try { languageStorage = window.localStorage; } catch { /* Storage can be disabled by the browser. */ }
setLanguage(initialLanguage(navigator.language || navigator.languages?.[0], languageStorage));

const $ = (id) => document.getElementById(id);
const manager = new ReceiverManager(navigator.hid);
const compatible = window.isSecureContext && Boolean(navigator.hid?.requestDevice);
let number = new Intl.NumberFormat(locale());
let selected = "", busy = false, renderTimer = null;
let candidate = null, pendingDump = null, dumpInfo = null, fileName = "";
const activity = [];

function element(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}

function message(text, success = false) {
  $("notice").textContent = text;
  $("notice").classList.toggle("success", success);
  $("notice").hidden = !text;
  localize($("notice"));
}

function renderActivity() {
  if (!activity.length) return;
  $("log").replaceChildren(...activity.map(({ message: text, level, time }) => {
    const item = element("li", level === "error" ? "error" : "");
    const stamp = element("time", "", new Date(time).toLocaleTimeString(locale(), { hour12: false }));
    stamp.dateTime = new Date(time).toISOString();
    item.append(stamp, document.createTextNode(text));
    return item;
  }));
  localize($("log"));
  $("log").scrollTop = $("log").scrollHeight;
}
function log({ message: text, level = "info", time = Date.now() }) {
  activity.push({ message: text, level, time });
  if (activity.length > 100) activity.shift();
  renderActivity();
}

function scheduleRender() {
  if (renderTimer === null) renderTimer = setTimeout(() => { renderTimer = null; render(); }, 100);
}

manager.addEventListener("change", scheduleRender);
manager.addEventListener("log", (event) => log(event.detail));

async function perform(operation) {
  if (busy) return;
  busy = true;
  message("");
  render();
  try { await operation(); }
  catch (error) {
    if (error.name === "NotFoundError") log({ message: "未选择设备，连接已取消。" });
    else { message(error.message || "操作失败"); log({ message: error.message || "操作失败", level: "error" }); }
  } finally { busy = false; render(); }
}

const cards = [];
for (let slot = 0; slot < 4; ++slot) {
  const card = element("article", "panel slot");
  const heading = element("div", "slot-heading");
  heading.append(element("h3", "", `手柄槽 ${slot + 1}`), element("span", "slot-number", `PIPE ${slot + 2}`));
  const state = element("span", "pill off", "未授权接口");
  const detail = element("div", "slot-detail");
  const raw = element("details");
  raw.append(element("summary", "", "最近输入"));
  const packet = element("p", "input-hex mono", "尚无输入");
  raw.append(packet);
  const actions = element("div", "actions");
  const commit = element("button", "", "保存绑定");
  commit.dataset.slot = String(slot);
  commit.setAttribute("aria-label", `保存手柄槽 ${slot + 1} 绑定`);
  const disconnect = element("button", "", "断开手柄");
  disconnect.setAttribute("aria-label", `断开手柄槽 ${slot + 1}`);
  const entry = () => manager.groups().find((group) => group.kind === "xiao" && group.id === selected)?.entries.find((item) => item.info.slot === slot);
  commit.addEventListener("click", () => perform(() => manager.enqueue(() => manager.commit(entry()))));
  disconnect.addEventListener("click", () => perform(() => manager.enqueue(() => manager.disconnect(entry()))));
  actions.append(commit, disconnect);
  card.append(heading, state, detail, raw, actions);
  $("slots").append(card);
  cards.push({ card, state, detail, packet, commit, disconnect });
}

const metricSpecs = [
  ["crc_ok", "射频 CRC 正确"], ["authenticated", "认证通过"], ["mic_failures", "MIC 失败"],
  ["late_ack", "ACK 逾期"], ["drops", "队列丢包"], ["storage_failures", "Flash 失败"],
];
const metricNodes = metricSpecs.map(([, label]) => {
  const node = element("div", "metric");
  const value = element("div", "metric-value", "—");
  const note = element("div", "metric-note", "等待采样");
  node.append(element("div", "metric-label", label), value, note);
  $("metrics").append(node);
  return { value, note };
});
const counterNodes = new Map();
for (const [key, , label] of COUNTERS) {
  const row = element("tr");
  const value = element("td", "", "—"), delta = element("td", "", "—");
  row.append(element("td", "", label), value, delta);
  $("counter-table").append(row);
  counterNodes.set(key, { value, delta });
}

function setOptions(select, values, placeholder) {
  const signature = JSON.stringify(values);
  if (select.dataset.signature === signature) return;
  const previous = select.value;
  select.replaceChildren(...(values.length ? values : [["", placeholder]]).map(([value, text]) => {
    const option = element("option", "", text);
    option.value = value;
    return option;
  }));
  if (values.some(([value]) => value === previous)) select.value = previous;
  select.dataset.signature = signature;
}

function queueSummaryLines(queue) {
  const c = queue.counters;
  return [
    `快照 ${queue.snapshotId} · MCU clock_us ${queue.snapshotClockUs} · 协议时钟${queue.clockAvailable ? "可用" : "不可用"}`,
    `触觉入队 / RF 队满 / USB 超时：${c.haptic_admitted_total} / ${c.haptic_full_total} / ${c.haptic_timeouts_total}`,
    `A2–A6 队满 ${c.a2_a6_full_total} · 全部控制重试 ${c.control_retries_total} · USB 触觉等待 ${c.haptic_waits_total}`,
    `RF / USB 高水位：${queue.rfHighWater}/${queue.rfCapacity} · ${queue.usbHighWater}/${queue.usbCapacity} · 触觉等待预算 ${queue.hapticWaitBudgetUs}µs`,
    `最大 FIFO / USB 年龄：${c.max_fifo_age_us} / ${c.max_usb_age_us}µs · RX 延迟 ${c.max_rx_latency_us}µs · 处理间隔 ${c.max_process_gap_us}µs`,
  ];
}

function renderQueue(group) {
  const supported = (group?.diagnostic?.pairingLink?.firmwareRevision ?? 0) >= 11;
  $("capture-queue").disabled = busy || !supported || !group.entries.some((entry) => !entry.channel.closed && !entry.queryError);
  const summary = group?.diagnostic?.queueDiagnostics;
  $("queue-summary").textContent = summary ? ["常规摘要（只读取 FC 页 0）", ...queueSummaryLines(summary)].join("\n") :
    "连接修订号 11 或更新的 XIAO 固件后，可读取冻结队列诊断。";
  const full = group?.queueSnapshot;
  $("queue-snapshot").textContent = full ? [
    `完整冻结快照 · 主机采集时间 ${full.capturedAt}`,
    ...queueSummaryLines(full),
    `当前 USB 深度 ${full.usbDepth} · 默认处理批量 ${full.usbBatch} · 事件 ${full.eventCount} 条`,
    ...full.slots.map((s) => [
      `槽 ${s.slot + 1}：触觉入队 / 队满 / 超时 ${s.haptic_admitted} / ${s.haptic_full} / ${s.usb_haptic_timeouts} · 待处理触觉 ${s.pendingHaptics}`,
      `  总入队 ${s.admitted} · A2/A3/A4/A5/A6 队满 ${s.a2_full}/${s.a3_full}/${s.a4_full}/${s.a5_full}/${s.a6_full} · 其他队满 ${s.other_full}`,
      `  初始 / 计数控制重试 ${s.initial_retries} / ${s.count_retries} · 提交等待 ${s.commit_waits} · 离线 ${s.offline}`,
      `  分片序列化 / 退役 / 清除 ${s.serialized} / ${s.retired} / ${s.cleared_fragments} · RF 高水位 ${s.fifoHighWater}`,
      `  最大 FIFO / lane / USB 年龄 ${s.max_fifo_age_us} / ${s.max_lane_age_us} / ${s.usb_max_age_us}µs`,
      `  USB 入队 / 处理 ${s.usb_admitted} / ${s.usb_processed} · 触觉收到 / 等待 / 过期 ${s.usb_haptic_received} / ${s.usb_haptic_waits} / ${s.usb_haptic_stale}`,
      `  USB 触觉离线 / 重置取消 ${s.usb_haptic_offline} / ${s.usb_haptic_reset_cancelled}`,
    ].join("\n")),
  ].join("\n") : "尚未读取完整快照；手动读取或下载诊断 JSON 时采集，常规刷新保留最近完整快照。";
  $("queue-events").replaceChildren(...(full?.events ?? []).map((event) => {
    const row = element("tr");
    for (const value of [event.id, event.at_us, event.generation, event.age_us, event.slot + 1, event.kindName,
      `0x${event.command.toString(16).padStart(2, "0")}`, event.length, event.needed, event.fifo_count,
      `0x${event.lane_flags.toString(16)}`, event.usb_depth]) row.append(element("td", "", String(value)));
    return row;
  }));
}

let mappingLoadedKey = "", modeShownKey = "";
function renderDiagonalAngle() {
  const angle = Number($("mapping-dpad-angle").value);
  $("mapping-dpad-angle-value").textContent = angle === 0 ? "0°（关闭斜向）" : `${angle}°（每个直向 ${90 - angle}°）`;
  localize($("mapping-dpad-angle-value"));
}
$("mapping-dpad-angle").addEventListener("input", renderDiagonalAngle);
const mappingSelects = MAPPING_SOURCES.map((name, index) => {
  const label = element("label", "", name), select = element("select");
  select.id = `mapping-button-${index}`; select.setAttribute("aria-label", `${name} 映射`);
  MAPPING_TARGETS.forEach((target, value) => { const option = element("option", "", target); option.value = value; select.append(option); });
  label.append(select); $("mapping-buttons").append(label); return select;
});
function fillMapping(mapping) {
  mappingSelects.forEach((select, i) => { select.value = mapping.bindings[i]; });
  $("mapping-stick-deadzone").value = mapping.stickDeadzone;
  $("mapping-pad-deadzone").value = mapping.padDeadzone;
  $("mapping-dpad-threshold").value = mapping.dpadThreshold;
  $("mapping-invert-x").checked = mapping.invertRightX;
  $("mapping-invert-y").checked = mapping.invertRightY;
  $("mapping-require-click").checked = mapping.requireLeftPadClick;
  $("mapping-persist-mode").checked = Boolean(mapping.persistMode);
  $("mapping-dpad-angle").value = mapping.diagonalAngle ?? 45;
  renderDiagonalAngle();
}
function readMappingForm() {
  const mapping = { bindings: mappingSelects.map((select) => Number(select.value)),
    stickDeadzone: $("mapping-stick-deadzone").valueAsNumber, padDeadzone: $("mapping-pad-deadzone").valueAsNumber,
    dpadThreshold: $("mapping-dpad-threshold").valueAsNumber, invertRightX: $("mapping-invert-x").checked,
    invertRightY: $("mapping-invert-y").checked, requireLeftPadClick: $("mapping-require-click").checked,
    persistMode: $("mapping-persist-mode").checked, diagonalAngle: $("mapping-dpad-angle").valueAsNumber };
  encodeInputMapping(mapping);
  return mapping;
}
function renderInputMode(group) {
  const state = group?.diagnostic?.inputMode;
  const supported = Boolean(state && group.entries.some((e) => !e.channel.closed && !e.queryError));
  $("input-mode-status").textContent = state ? `当前：${INPUT_MODES[state.mode]} · 输入来源：${state.ownerSlot === null ? "等待手柄" : `槽 ${state.ownerSlot + 1}`} · ${state.persistMode ? "已记住模式，重新上电保持最后选择" : "默认重新上电回到 Steam"}` : "需要修订号 12 或更新的 XIAO 固件；Xbox / PS5 / Switch / Lizard 无网页接口，请 A + Steam 开机返回。";
  if (state?.switchSaveFailed) $("input-mode-status").textContent += " · 启动选择保存失败，未重启；请重新请求或检查 Flash。";
  if (state?.mode === 1) {
    const x = state.xinput;
    $("input-mode-status").textContent += x ? ` · XInput 端点：${x.opened ? "已打开" : "未打开"} · IN 提交/完成/失败：${x.inSubmitted}/${x.inCompleted}/${x.inFailed}（本次上电累计；Windows 识别需另行确认）`
      : " · 此修订号没有端点诊断，模式标签不能确认 Windows 已识别手柄。";
  }
  const key = state ? `${group.id}:${state.mode}` : "";
  if (key !== modeShownKey) { if (state) $("input-mode").value = state.mode; modeShownKey = key; }
  $("input-mode").disabled = $("switch-input-mode").disabled = busy || !supported;
  // Selecting an intention must not depend on an old diagnostic snapshot.
  // switchInputMode re-reads FD and rejects unsupported writes at execution.
  $("input-mode-ps5").disabled = false;
  $("input-mode-availability").textContent = supported ?
    `最近读取的固件修订号：${state.firmwareRevision}。PS5需要17、Switch需要18、Lizard需要19或更新；点击切换时重新校验设备版本。` :
    state ? "Steam配置接口查询异常或已关闭，请刷新状态或重新连接；单个模式不会由缓存版本锁死。" :
    "尚未读取到输入模式，请在Steam模式连接配置接口。PS5需要17、Switch需要18、Lizard需要19或更新固件。";
  $("load-input-mapping").disabled = busy || !supported;
  const mapping = group?.inputMapping;
  const mappingKey = mapping ? `${group.id}:${mapping.token}` : "";
  if (mappingKey !== mappingLoadedKey) {
    if (mapping) { fillMapping(mapping); $("mapping-status").textContent = "已读取接收器配置，修改后点击保存。"; }
    mappingLoadedKey = mappingKey;
  }
  $("mapping-controls").disabled = $("default-input-mapping").disabled = busy || !supported || !mapping;
  $("mapping-persist-mode").disabled = !supported || !(state.firmwareRevision >= 16);
  $("mapping-dpad-angle").disabled = !supported || !(state.firmwareRevision >= 16);
  $("save-input-mapping").disabled = busy || !supported || !mapping || !state.mappingPersistence;
}

function render() {
  const groups = manager.groups();
  const xiaos = groups.filter((group) => group.kind === "xiao");
  const donors = groups.filter((group) => group.kind === "original");
  if (!xiaos.some((group) => group.id === selected)) selected = xiaos[0]?.id ?? "";
  setOptions($("receiver-select"), xiaos.map((group) => [group.id, `${group.id.toUpperCase()} · ${group.entries.length}/4 接口`]), "未连接 XIAO");
  $("receiver-select").value = selected;
  setOptions($("donor-select"), donors.map((group) => [group.id, `${group.id.toUpperCase()} · 原版身份`]), "未连接原版接收器");
  const group = xiaos.find((item) => item.id === selected);
  const diagnostic = group?.diagnostic;
  const previous = group?.previous;
  $("connection-status").textContent = groups.length ? `${xiaos.length} 只 XIAO，${donors.length} 只原版身份来源 · ${busy ? "正在操作…" : "USB 接口已连接"}` : "尚未连接接收器";
  $("connect").disabled = busy || !compatible;
  $("restore").disabled = busy || !compatible;
  $("close").disabled = busy || !groups.length;
  $("receiver-select").disabled = busy || !xiaos.length;
  $("donor-select").disabled = busy || !donors.length;
  $("use-donor").disabled = busy || !donors.length;
  for (const id of ["pair", "stop-pair", "refresh", "backup", "export-diagnostics"]) $(id).disabled = busy || !group;
  const revision = diagnostic?.pairingLink?.firmwareRevision ?? 0;
  $("bootloader").disabled = busy || !group || revision < 5 || group.entries.every((item) => item.channel.closed);
  $("firmware-status").textContent = !group ? "连接修订号 5 或更新的 XIAO 固件后，可从网页进入刷入模式。" : revision < 5 ?
    "当前固件不支持网页进入刷入模式。首次升级请双击板卡 RESET，复制修订号 5 的 UF2。" :
    `目标 ${group.id.toUpperCase()} · 修订号 ${revision} · 支持网页进入 UF2 刷入模式。`;
  $("backup-file").disabled = busy;
  $("auto-commit").disabled = busy;
  const connected = Boolean((diagnostic?.activePipes ?? 0) & 0x3c) || group?.entries.some((item) => item.state === 2 || item.state === 3);
  $("import").disabled = busy || !candidate || !group || connected;
  $("import").title = connected ? "请先关闭或断开所有无线手柄" : "替换目标接收器身份及全部绑定，提交后回读核验";
  const pairing = Boolean(diagnostic?.pairing);
  $("pairing-indicator").textContent = pairing ? "配对已开启" : "未开启配对";
  $("pairing-indicator").className = pairing ? "pill good" : "pill off";
  for (const [slot, nodes] of cards.entries()) {
    const entry = group?.entries.find((item) => item.info.slot === slot);
    nodes.state.textContent = entry ? STATE_NAMES[entry.state] ?? "等待状态" : "未授权接口";
    nodes.state.className = `pill ${entry?.state === 2 ? "good" : entry?.state === 3 ? "pending" : "off"}`;
    nodes.detail.textContent = !entry ? "再次授权接收器的 vendor HID 接口后，此槽可用于控制和诊断。" :
      `${number.format(entry.inputCount)} 个输入报告${entry.lastInputAt ? ` · 最近输入 ${Math.max(0, Math.floor((Date.now() - entry.lastInputAt) / 1000))} 秒前` : " · 尚无输入活动"}`;
    if (entry?.queryError) nodes.detail.textContent += ` · 查询异常（无线状态为最近记录）：${entry.queryError}`;
    nodes.packet.textContent = entry?.lastInput ? `序号 ${entry.lastInput.sequence}\n${entry.lastInput.hex}` : "尚无输入";
    nodes.commit.disabled = busy || entry?.state !== 3 || entry.channel.closed;
    nodes.disconnect.disabled = busy || !entry || entry.channel.closed || entry.state === 1;
  }
  const elapsed = diagnostic && previous ? (diagnostic.timestamp - previous.timestamp) / 1000 : 0;
  const metricValue = (sample, key) => !sample ? null : key === "drops" ? sample.counters.queue_full + sample.counters.radio_queue_full + sample.counters.usb_command_drops : sample.counters[key];
  metricSpecs.forEach(([key], index) => {
    const value = metricValue(diagnostic, key);
    const delta = value === null ? null : counterDelta(value, metricValue(previous, key));
    metricNodes[index].value.textContent = value === null ? "—" : number.format(value);
    metricNodes[index].note.textContent = delta === null || elapsed <= 0 ? "等待下一次采样" :
      key === "crc_ok" || key === "authenticated" ? `${(delta / elapsed).toFixed(1)} / 秒` : `本次 +${number.format(delta)}`;
  });
  for (const [key, nodes] of counterNodes) {
    const value = diagnostic?.counters[key];
    const delta = value === undefined ? null : counterDelta(value, previous?.counters[key]);
    nodes.value.textContent = value === undefined ? "—" : number.format(value);
    nodes.delta.textContent = delta === null ? "—" : `+${number.format(delta)}`;
  }
  $("hints").replaceChildren(...diagnosticHints(diagnostic, previous).map((text) => element("li", "", text)));
  $("updated").textContent = diagnostic ? `采样 ${new Date(diagnostic.timestamp).toLocaleTimeString(locale(), { hour12: false })} · pipe mask 0x${diagnostic.activePipes.toString(16)}` : "尚无采样";
  const link = diagnostic?.pairingLink;
  const byteHex = (value) => `0x${value.toString(16).padStart(2, "0")}`;
  $("pairing-link").textContent = link ? [
    `固件修订号 ${link.firmwareRevision} · F0 v${diagnostic.version} · 当前信道 ${link.channel}`,
    `pipe0..5 收包：${link.pipeRx.join(" / ")}`,
    `ACK 安排 / TX READY / 完成：${link.ackScheduled} / ${link.ackReady} / ${link.ackCompleted}`,
    `无需 ACK / 零载荷 ACK：${link.ackSkipped} / ${link.emptyAck}`,
    link.lastRx ? `最近 RX：pipe${link.lastRx.pipe} ch${link.lastRx.channel} len${link.lastRx.length} S1=${byteHex(link.lastRx.s1)} H=${byteHex(link.lastRx.header)} 请求ACK=${link.lastRx.ackRequested}` : "尚无 RX",
    `最大 IRQ 处理延迟 ${link.maxIrqDelayUs}µs · 最近 RX END→TX READY 处理 ${link.lastTxReadyDelayUs}µs`,
  ].join("\n") : "当前没有 F7 链路报告；请刷入修订号 7 / 诊断协议 v3 固件。";
  const reconnect = diagnostic?.reconnect;
  $("reconnect-detail").textContent = reconnect ? [
    `已保存绑定 ${reconnect.bondCount} 条 · bootstrap 记录 ${reconnect.bootstrapRecords} 条`,
    `最近接入结果：${RECONNECT_OUTCOMES[reconnect.lastOutcome] ?? reconnect.lastOutcome}`,
    `查询命中 / 未命中：${reconnect.counters.lookup_hits} / ${reconnect.counters.lookup_misses}`,
    `H=2 请求 / 恢复成功 / MIC失败：${reconnect.counters.restore_requests} / ${reconnect.counters.restore_accepted} / ${reconnect.counters.restore_mic_failures}`,
    `保存请求 / 延后 / 成功 / 失败：${reconnect.counters.commit_requests} / ${reconnect.counters.commit_deferred} / ${reconnect.counters.commit_successes} / ${reconnect.counters.commit_failures}`,
    `未保存即断开 ${reconnect.counters.unsaved_disconnects} 次 · 无查询缓存恢复 ${reconnect.counters.restore_fallbacks} 次`,
    `待保存 / 待最终key / 已请求保存槽mask：${byteHex(reconnect.pendingMask)} / ${byteHex(reconnect.keyNotReadyMask)} / ${byteHex(reconnect.commitRequestedMask)}`,
  ].join("\n") : "修订号 4 的 F8 报告可区分空绑定库、查询未命中、恢复认证失败，以及等待最终密钥保存。";
  const host = diagnostic?.hostLink;
  $("host-detail").textContent = host ? [
    `最近主机 SET：槽 ${host.lastSet.slot + 1} ${byteHex(host.lastSet.command)} · ${host.lastSet.size} 字节`,
    `最近主机 GET：槽 ${host.lastGet.slot + 1} ${byteHex(host.lastGet.command)} · payload ${host.lastGet.payloadLength} 字节`,
    `SET / GET：${host.counters.sets} / ${host.counters.gets}`,
    `查询待就绪重试 / 就绪确认：${host.counters.query_busy} / ${host.counters.query_ready}`,
    `尚未发布 GET / 无效 SET / 命令丢弃：${host.counters.unpublished_gets} / ${host.counters.invalid_sets} / ${host.counters.command_drops}`,
    ...(host.lastRejected ? [`最近拒绝：槽 ${host.lastRejected.slot + 1} ${byteHex(host.lastRejected.command)} · 声明参数 ${host.lastRejected.declaredLength} 字节 · ${host.lastRejected.reasonName}`] : []),
    ...host.slots.map((slot) => `槽 ${slot.slot + 1}：查询 ${byteHex(slot.queryCommand)} 状态 ${["无", "等待无线回复", "回复就绪", "无线离线"][slot.queryStatus]} · 缓存 ${byteHex(slot.replyCommand)} / ${slot.replyLength}`),
  ].join("\n") : "修订号 6 的 FA 报告记录主机查询就绪状态与 USB 重试计数，不含绑定密钥。";
  const transport = diagnostic?.transportLink;
  $("transport-detail").textContent = transport ? [
    `断开计数 · 看门狗 ${transport.counters.watchdog_disconnects} / 主机 B2 ${transport.counters.host_disconnects} / 会话替换 ${transport.counters.replaced_disconnects}`,
    `换钥加密失败 ${transport.counters.crypto_disconnects} / 清除绑定 ${transport.counters.erase_disconnects}`,
    `等待认证相位的载荷 ACK ${transport.counters.ack_deferred} · 已丢弃旧会话 SET ${transport.counters.stale_session_sets}`,
    ...transport.slots.map((slot) => `槽 ${slot.slot + 1}：最近原因 ${slot.reasonName} · ${slot.connected ? "当前" : "断开前"} watchdog ${slot.watchdog} / 队列 ${slot.queued} / lane ${byteHex(slot.laneMask)} / phase ${byteHex(slot.phaseMask)} / RX低8位 ${slot.rxCounterLow8} / TX低8位 ${slot.txCounterLow8}`),
  ].join("\n") : "修订号 10 的 FB 报告区分无线超时、主机断开、会话替换与换钥失败，并保留断开前的传输状态。";
  renderQueue(group);
  renderInputMode(group);
  localize(document);
}

function download(value, name) {
  const blob = new Blob([JSON.stringify(value, null, 2) + "\n"], { type: "application/json" });
  const url = URL.createObjectURL(blob);
  const anchor = element("a");
  anchor.href = url;
  anchor.download = name;
  document.body.append(anchor);
  anchor.click();
  anchor.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

function preview() {
  if (!candidate) { $("backup-preview").textContent = "尚未载入可用绑定文件"; localize($("backup-preview")); return; }
  const lines = [`文件：${fileName}`, `接收器身份：${candidate.receiverAddress.toUpperCase()}`, `绑定：${candidate.bonds.length} 条`];
  if (dumpInfo) {
    lines.push(`启动选择页：0x${dumpInfo.activePage.toString(16)} · 同 ID 旧记录 ${dumpInfo.superseded} 条`);
    for (const page of dumpInfo.pages) lines.push(`0x${page.address.toString(16)}：F0 ${page.valid} / 空白 ${page.empty} / 异常 ${page.abnormal}`);
    lines.push(dumpInfo.integrity);
  }
  for (const bond of candidate.bonds) lines.push(`手柄 ${bond.controllerId.toUpperCase()}`);
  $("backup-preview").textContent = lines.join("\n");
  localize($("backup-preview"));
}

function reparseDump() {
  candidate = null;
  dumpInfo = parseJournalDump(pendingDump, $("import-address").value.trim());
  candidate = dumpInfo.backup;
  preview();
}

$("connect").addEventListener("click", () => perform(() => manager.request()));
$("restore").addEventListener("click", () => perform(() => manager.restore()));
$("close").addEventListener("click", () => perform(() => manager.close()));
$("receiver-select").addEventListener("change", () => { selected = $("receiver-select").value; render(); });
$("refresh").addEventListener("click", () => perform(() => manager.enqueue(() => manager.refresh())));
$("capture-queue").addEventListener("click", () => perform(() => manager.enqueue(() => manager.captureQueueSnapshot(selected))));
$("switch-input-mode").addEventListener("click", () => perform(() => manager.enqueue(async () => {
  const result = await manager.switchInputMode(selected, Number($("input-mode").value));
  message(result.firmwareRevision >= 16 && result.requestedMode !== 0 ?
    `请求已确认，接收器保存启动选择后将重启进入 ${["Steam","Xbox","PS5","Switch","Lizard"][result.requestedMode]}。${["Steam","Xbox","PS5","Switch","Lizard"][result.requestedMode]} 无网页接口；A + Steam 开机返回 Steam 后再连接网页。` :
    "模式请求已确认，等待接收器启动后连接 / 授权接口。", true);
})));
$("load-input-mapping").addEventListener("click", () => perform(() => manager.enqueue(async () => {
  await manager.loadInputMapping(selected); mappingLoadedKey = "";
})));
$("default-input-mapping").addEventListener("click", () => {
  fillMapping(defaultInputMapping(manager.inputMappings.get(selected)?.firmwareRevision ?? 16));
  $("mapping-status").textContent = "已填入默认映射，点击保存后生效。";
  localize($("mapping-status"));
});
$("save-input-mapping").addEventListener("click", () => perform(() => manager.enqueue(async () => {
  await manager.saveInputMapping(selected, readMappingForm());
  message("Xbox 按键配置已保存，重插 Type-C 后也会保留。", true);
})));
$("pair").addEventListener("click", () => perform(() => manager.enqueue(() => manager.pairing(selected, true, $("auto-commit").checked))));
$("stop-pair").addEventListener("click", () => perform(() => manager.enqueue(() => manager.pairing(selected, false))));
$("bootloader").addEventListener("click", () => perform(() => manager.enqueue(async () => {
  await manager.bootloader(selected);
  message("接收器已确认刷入模式请求。等待 UF2 磁盘出现后复制 XIAO UF2 文件，刷入后重新连接网页。", true);
})));
$("backup").addEventListener("click", () => perform(() => manager.enqueue(async () => {
  const backup = await manager.backup(selected);
  download(backup, `sc2015-bindings-${backup.receiverAddress}.json`);
  log({ message: `已备份 ${backup.bonds.length} 条绑定，密钥只写入本地下载文件。` });
})));
$("use-donor").addEventListener("click", () => {
  const id = $("donor-select").value;
  if (!id) return;
  $("import-address").disabled = false;
  $("import-address").value = id;
  if (pendingDump) {
    try { reparseDump(); } catch (error) { message(error.message); }
  } else { candidate = null; dumpInfo = null; $("backup-file").value = ""; preview(); }
  render();
});
$("import-address").addEventListener("input", () => {
  if (pendingDump) {
    try { reparseDump(); }
    catch (error) { $("backup-preview").textContent = error.message; }
    render();
  }
});
$("backup-file").addEventListener("change", () => perform(async () => {
  candidate = pendingDump = dumpInfo = null;
  preview();
  const file = $("backup-file").files[0];
  if (!file) { preview(); return; }
  if (file.size > 65536) throw new ProtocolError("绑定文件超过 64 KiB 限制");
  fileName = file.name;
  const bytes = new Uint8Array(await file.arrayBuffer());
  if (file.name.toLowerCase().endsWith(".json")) {
    let parsed;
    try { parsed = JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(bytes)); }
    catch { throw new ProtocolError("绑定 JSON 解析失败"); }
    candidate = validateBackup(parsed);
    $("import-address").value = candidate.receiverAddress;
    $("import-address").disabled = true;
    preview();
  } else {
    $("import-address").disabled = false;
    pendingDump = bytes;
    reparseDump();
  }
  $("import-progress").textContent = "导入会在完整传输后一次提交，并回读核验。";
}));
$("import").addEventListener("click", () => perform(() => manager.enqueue(async () => {
  if (!candidate) throw new ProtocolError("请先载入绑定备份或转储");
  const result = await manager.import(selected, candidate, (count, total) => {
    $("import-progress").textContent = `已暂存 ${count} / ${total} 条，等待完整提交…`;
    localize($("import-progress"));
  });
  selected = result.receiverAddress;
  candidate = pendingDump = dumpInfo = null;
  $("backup-file").value = "";
  $("import-address").disabled = false;
  $("import-address").value = result.receiverAddress;
  $("backup-preview").textContent = `已应用 ${result.bonds.length} 条绑定并回读一致。`;
  $("import-progress").textContent = "请拔下原接收器，再启动手柄测试恢复连接。";
  message("绑定和接收器身份已提交；Flash 回读核验通过。", true);
})));
$("export-diagnostics").addEventListener("click", () => perform(() => manager.enqueue(async () => {
  const failures = new Map();
  for (const group of manager.groups()) {
    if (group.kind !== "xiao" || (group.diagnostic?.pairingLink?.firmwareRevision ?? 0) < 11) continue;
    try { await manager.captureQueueSnapshot(group.id); }
    catch (error) { failures.set(group.id, error.message); }
  }
  const groups = manager.groups().map((group) => ({ receiverId: group.id, kind: group.kind,
    diagnostics: group.diagnostic, queueSnapshot: group.queueSnapshot, inputMapping: group.inputMapping,
    ...(failures.has(group.id) ? { queueSnapshotError: failures.get(group.id) } : {}),
    slots: group.entries.map((entry) => ({ slot: entry.info.slot,
      state: entry.state, queryError: entry.queryError, queryClosed: entry.channel.closed,
      inputCount: entry.inputCount, lastInputAt: entry.lastInputAt, lastInput: entry.lastInput })) }));
  download({ format: "sc2015-diagnostics", version: 1, createdAt: new Date().toISOString(), groups, activity }, "sc2015-diagnostics.json");
  if (failures.size) message("部分完整队列快照采集失败，JSON 已标记错误；如有历史快照，保留其原始采集时间。");
})));
$("clear-log").addEventListener("click", () => { activity.length = 0; $("log").replaceChildren(element("li", "muted", "操作记录已清空。")); localize($("log")); });

$("language").value = getLanguage();
$("language").addEventListener("change", () => {
  setLanguage($("language").value, languageStorage);
  number = new Intl.NumberFormat(locale());
  renderActivity();
  renderDiagonalAngle();
  render();
});

async function polling() {
  if (!busy && compatible && $("poll").checked && !document.hidden && manager.entries.size) {
    try { await manager.enqueue(() => manager.refresh()); }
    catch (error) { log({ message: error.message, level: "error" }); }
  }
  setTimeout(polling, 1000);
}

if (!window.isSecureContext) message("请通过 GitHub Pages HTTPS 地址或 localhost 打开；file:// 和普通 HTTP 不能用于 WebHID。");
else if (!navigator.hid?.requestDevice) message("当前浏览器不支持 WebHID。请使用桌面版 Chrome 或 Edge。");
$("environment").textContent = compatible ? "WebHID 可用 · 诊断协议 v2/v3" : "当前环境不支持 WebHID";
window.addEventListener("pagehide", () => { manager.destroy().catch(() => {}); });
render();
setTimeout(polling, 1000);
