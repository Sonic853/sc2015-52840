import { COUNTERS, STATE_NAMES, ProtocolError, validateBackup, parseJournalDump,
  counterDelta, diagnosticHints, RECONNECT_OUTCOMES } from "./protocol.mjs";
import { ReceiverManager } from "./manager.mjs";

const $ = (id) => document.getElementById(id);
const manager = new ReceiverManager(navigator.hid);
const compatible = window.isSecureContext && Boolean(navigator.hid?.requestDevice);
const number = new Intl.NumberFormat("zh-CN");
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
}

function log({ message: text, level = "info", time = Date.now() }) {
  if (!activity.length) $("log").replaceChildren();
  activity.push({ message: text, level, time });
  if (activity.length > 100) activity.shift();
  const item = element("li", level === "error" ? "error" : "");
  const stamp = element("time", "", new Date(time).toLocaleTimeString("zh-CN", { hour12: false }));
  stamp.dateTime = new Date(time).toISOString();
  item.append(stamp, document.createTextNode(text));
  $("log").append(item);
  while ($("log").children.length > 100) $("log").firstChild.remove();
  $("log").scrollTop = $("log").scrollHeight;
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
  $("updated").textContent = diagnostic ? `采样 ${new Date(diagnostic.timestamp).toLocaleTimeString("zh-CN", { hour12: false })} · pipe mask 0x${diagnostic.activePipes.toString(16)}` : "尚无采样";
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
    ...host.slots.map((slot) => `槽 ${slot.slot + 1}：查询 ${byteHex(slot.queryCommand)} 状态 ${["无", "等待无线回复", "回复就绪", "无线离线"][slot.queryStatus]} · 缓存 ${byteHex(slot.replyCommand)} / ${slot.replyLength}`),
  ].join("\n") : "修订号 6 的 FA 报告记录主机查询就绪状态与 USB 重试计数，不含绑定密钥。";
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
  if (!candidate) { $("backup-preview").textContent = "尚未载入可用绑定文件"; return; }
  const lines = [`文件：${fileName}`, `接收器身份：${candidate.receiverAddress.toUpperCase()}`, `绑定：${candidate.bonds.length} 条`];
  if (dumpInfo) {
    lines.push(`启动选择页：0x${dumpInfo.activePage.toString(16)} · 同 ID 旧记录 ${dumpInfo.superseded} 条`);
    for (const page of dumpInfo.pages) lines.push(`0x${page.address.toString(16)}：F0 ${page.valid} / 空白 ${page.empty} / 异常 ${page.abnormal}`);
    lines.push(dumpInfo.integrity);
  }
  for (const bond of candidate.bonds) lines.push(`手柄 ${bond.controllerId.toUpperCase()}`);
  $("backup-preview").textContent = lines.join("\n");
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
$("export-diagnostics").addEventListener("click", () => {
  const groups = manager.groups().map((group) => ({ receiverId: group.id, kind: group.kind,
    diagnostics: group.diagnostic, slots: group.entries.map((entry) => ({ slot: entry.info.slot,
      state: entry.state, queryError: entry.queryError, queryClosed: entry.channel.closed,
      inputCount: entry.inputCount, lastInputAt: entry.lastInputAt, lastInput: entry.lastInput })) }));
  download({ format: "sc2015-diagnostics", version: 1, createdAt: new Date().toISOString(), groups, activity }, "sc2015-diagnostics.json");
});
$("clear-log").addEventListener("click", () => { activity.length = 0; $("log").replaceChildren(element("li", "muted", "操作记录已清空。")); });

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
