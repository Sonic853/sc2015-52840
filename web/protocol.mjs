export const FILTER = Object.freeze({ vendorId: 0x28de, productId: 0x1142, usagePage: 0xff00, usage: 1 });
export const COMMAND = Object.freeze({ IDENTITY: 0xa1, PAIR: 0xad, DISCONNECT: 0xb2, COMMIT: 0xb3,
  STATE: 0xb4, DIAGNOSTICS: 0xf0, SNAPSHOT: 0xf1, RECORD: 0xf2, STAGE: 0xf3,
  WRITE: 0xf4, APPLY: 0xf5, RELEASE: 0xf6, PAIRING_LINK: 0xf7, RECONNECT: 0xf8 });
export const COUNTERS = Object.freeze([
  ["rx", 5, "应用接收"], ["authenticated", 9, "认证通过"], ["mic_failures", 13, "MIC 失败"],
  ["duplicates", 17, "已认证重复"], ["invalid", 21, "无效消息"], ["queue_full", 25, "协议队列满"],
  ["storage_failures", 29, "Flash 失败"], ["crc_ok", 35, "射频 CRC 正确"], ["crc_bad", 39, "射频 CRC 错误"],
  ["radio_queue_full", 43, "射频队列满"], ["late_ack", 47, "ACK 逾期"], ["radio_faults", 51, "RADIO 故障"],
  ["usb_command_drops", 55, "USB 命令丢弃"],
]);
export const STATE_NAMES = Object.freeze({ 1: "未连接", 2: "已连接 · 绑定已保存", 3: "已连接 · 绑定待保存" });
export const RECONNECT_COUNTERS = Object.freeze([
  "lookup_hits", "lookup_misses", "restore_requests", "restore_accepted", "restore_mic_failures",
  "new_pairs", "key_updates", "commit_requests", "commit_deferred", "commit_successes",
  "commit_failures", "unsaved_disconnects", "restore_fallbacks",
]);
export const RECONNECT_OUTCOMES = Object.freeze({ 0: "尚无接入", 1: "绑定查询命中", 2: "绑定查询未命中",
  3: "新配对未开启", 4: "没有已保存绑定", 5: "接入 MIC 不匹配", 6: "接收器地址不匹配",
  7: "手柄标识不匹配", 8: "新配对接入成功", 9: "已保存绑定恢复成功", 10: "正在导入", 11: "没有空槽" });
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

export class ProtocolError extends Error {
  constructor(message) { super(message); this.name = "ProtocolError"; }
}

export function hex(bytes) {
  return Array.from(bytes, (value) => value.toString(16).padStart(2, "0")).join("");
}

export function unhex(value, size) {
  if (typeof value !== "string" || value.length !== size * 2 || !/^[0-9a-f]+$/i.test(value)) {
    throw new ProtocolError(`需要 ${size * 2} 位十六进制字节串`);
  }
  return Uint8Array.from(value.match(/../g), (byte) => Number.parseInt(byte, 16));
}

export function featureBytes(data) {
  let bytes;
  if (data instanceof DataView) bytes = new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
  else if (data instanceof Uint8Array) bytes = data;
  else if (data instanceof ArrayBuffer) bytes = new Uint8Array(data);
  else throw new ProtocolError("HID 返回的数据类型不正确");
  if (bytes.length === 65 && bytes[0] === 0) bytes = bytes.subarray(1);
  if (bytes.length !== 64) throw new ProtocolError(`HID 报告应为 64 字节，实际 ${bytes.length}`);
  return bytes.slice();
}

export function commandBytes(command, args = []) {
  if (!Number.isInteger(command) || command < 0 || command > 255 || args.length > 62 ||
      !Array.from(args).every((value) => Number.isInteger(value) && value >= 0 && value <= 255)) {
    throw new ProtocolError("Feature 命令参数无效");
  }
  const bytes = new Uint8Array(64);
  bytes.set([command, args.length]);
  bytes.set(args, 2);
  return bytes;
}

export function isReceiver(device) {
  const vendor = (collections) => (collections ?? []).some((collection) =>
    (collection.usagePage === 0xff00 && collection.usage === 1) || vendor(collection.children));
  return device.vendorId === FILTER.vendorId && device.productId === FILTER.productId && vendor(device.collections);
}

export class FeatureChannel {
  #tail = Promise.resolve();
  #closed = false;
  constructor(device, { timeout = 1800, attempts = 50, delay = 12, wait = sleep } = {}) {
    this.device = device;
    this.timeout = timeout;
    this.attempts = attempts;
    this.delay = delay;
    this.wait = wait;
  }
  get closed() { return this.#closed; }
  async #bounded(operation, milliseconds) {
    let timer;
    try {
      return await Promise.race([Promise.resolve().then(operation), new Promise((_, reject) => {
        timer = setTimeout(() => {
          this.#closed = true;
          try { Promise.resolve(this.device.close()).catch(() => {}); } catch {}
          reject(new ProtocolError("USB 请求超时；请重新连接接收器"));
        }, Math.max(1, milliseconds));
      })]);
    } finally { clearTimeout(timer); }
  }
  async open() {
    if (this.#closed) throw new ProtocolError("接口已关闭");
    if (!this.device.opened) await this.#bounded(() => this.device.open(), this.timeout);
  }
  exchange(command, args = [], ready = () => true) {
    const payload = commandBytes(command, args);
    const run = this.#tail.then(async () => {
      if (this.#closed || !this.device.opened) throw new ProtocolError("接收器接口已断开");
      const deadline = Date.now() + this.timeout;
      // WebHID receives report ID separately. The 64-byte payload starts at
      // the command; HIDAPI's extra zero prefix must not be sent here.
      await this.#bounded(() => this.device.sendFeatureReport(0, payload), deadline - Date.now());
      for (let attempt = 0; attempt < this.attempts; ++attempt) {
        await this.wait(this.delay);
        if (this.#closed) throw new ProtocolError("接收器接口已断开");
        if (Date.now() >= deadline) break;
        const data = await this.#bounded(() => this.device.receiveFeatureReport(0), deadline - Date.now());
        const reply = featureBytes(data);
        if (reply[0] === command && ready(reply)) return reply;
      }
      throw new ProtocolError("未收到完整 Feature 回复；请先退出同时访问接收器的 Steam 或其他工具，再重试");
    });
    this.#tail = run.catch(() => {});
    return run;
  }
  async close() {
    this.#closed = true;
    if (this.device.opened) await this.#bounded(() => this.device.close(), 1000);
  }
}

export function parseIdentity(report) {
  const bytes = featureBytes(report);
  const text = new TextDecoder().decode(bytes.subarray(6, 16));
  if (bytes[0] !== COMMAND.IDENTITY || bytes[1] !== 34 || !/^[0-9a-f]{10}$/i.test(text)) {
    throw new ProtocolError("A1 接收器身份报告无效");
  }
  const raw = unhex(text, 5);
  const address = raw.slice();
  address[0] &= 0xf8;
  const nameBytes = bytes.subarray(16, 36);
  const end = nameBytes.findIndex((value) => value === 0 || value === 255);
  const name = new TextDecoder().decode(nameBytes.subarray(0, end < 0 ? 20 : end));
  return { receiverId: hex(address), rawChipId: hex(raw), name, isXiao: name === "SC2015 XIAO", build: hex(bytes.subarray(2, 6)) };
}

export async function readIdentity(channel) {
  const reply = await channel.exchange(COMMAND.IDENTITY, [], (bytes) => bytes[1] === 34);
  return parseIdentity(reply);
}

export function parseDiagnostics(report) {
  const bytes = featureBytes(report);
  if (bytes[0] !== COMMAND.DIAGNOSTICS) throw new ProtocolError("不是 F0 诊断报告");
  if (bytes[2] !== 2 && bytes[2] !== 3) throw new ProtocolError("需要诊断协议 v2 或更新固件；请刷入本工程更新后的 XIAO UF2");
  if (bytes[1] !== 62 || bytes[33] >= 4 || bytes[34] !== 4 || (bytes[59] & 7)) {
    throw new ProtocolError("F0 槽号或接收器标识无效");
  }
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const counters = Object.fromEntries(COUNTERS.map(([name, offset]) => [name, view.getUint32(offset, true)]));
  return { version: bytes[2], slot: bytes[33], slots: 4, receiverId: hex(bytes.subarray(59, 64)),
    activePipes: bytes[3], pairing: Boolean(bytes[4]), counters, timestamp: Date.now() };
}

export async function readDiagnostics(channel) {
  const reply = await channel.exchange(COMMAND.DIAGNOSTICS, [], (bytes) => bytes[1] >= 57 && bytes[2] !== 0);
  const diagnostics = parseDiagnostics(reply);
  if (diagnostics.version >= 3) {
    const link = await channel.exchange(COMMAND.PAIRING_LINK, [], (bytes) => bytes[1] === 62 && bytes[2] === 1);
    diagnostics.pairingLink = parsePairingLink(link);
    if (diagnostics.pairingLink.firmwareRevision >= 4) {
      const reconnect = await channel.exchange(COMMAND.RECONNECT, [], (bytes) => bytes[1] === 62 && bytes[2] === 1);
      diagnostics.reconnect = parseReconnect(reconnect);
    }
  }
  return diagnostics;
}

export function parsePairingLink(report) {
  const bytes = featureBytes(report);
  if (bytes[0] !== COMMAND.PAIRING_LINK || bytes[1] !== 62 || bytes[2] !== 1 || bytes[3] < 3 || bytes[63] !== bytes[3]) {
    throw new ProtocolError("F7 配对链路报告无效");
  }
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const pipeRx = Array.from({ length: 6 }, (_, index) => view.getUint32(12 + 4 * index, true));
  return { version: 1, firmwareRevision: bytes[3], pipeRx,
    lastRx: bytes[6] ? { pipe: bytes[4], channel: bytes[5], length: bytes[6], s1: bytes[7],
      header: bytes[8], ackRequested: Boolean(bytes[7] & 1) } : null,
    lastAckLength: bytes[9], lastTxPipe: bytes[10], hfClock: bytes[11],
    ackScheduled: view.getUint32(36, true), ackReady: view.getUint32(40, true),
    ackCompleted: view.getUint32(44, true), ackSkipped: view.getUint32(48, true),
    emptyAck: view.getUint32(52, true), maxIrqDelayUs: view.getUint16(56, true),
    lastTxReadyDelayUs: view.getUint16(58, true), radioState: bytes[60], channel: bytes[61], ppi: bytes[62] };
}

export function parseReconnect(report) {
  const bytes = featureBytes(report);
  if (bytes[0] !== COMMAND.RECONNECT || bytes[1] !== 62 || bytes[2] !== 1 ||
      bytes[3] < 4 || bytes[63] !== bytes[3] || bytes[4] > 20 || bytes[5] > bytes[4] ||
      bytes[60] > 15 || bytes[61] > 15 || bytes[62] > 15) {
    throw new ProtocolError("F8 重连诊断报告无效");
  }
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  return { version: 1, firmwareRevision: bytes[3], bondCount: bytes[4], bootstrapRecords: bytes[5],
    lookupValid: Boolean(bytes[6]), lastOutcome: bytes[7],
    counters: Object.fromEntries(RECONNECT_COUNTERS.map((name, index) => [name, view.getUint32(8 + 4 * index, true)])),
    pendingMask: bytes[60], keyNotReadyMask: bytes[61], commitRequestedMask: bytes[62] };
}

export async function readState(channel) {
  const reply = await channel.exchange(COMMAND.STATE, [], (bytes) => bytes[1] === 1 && STATE_NAMES[bytes[2]]);
  return reply[2];
}

export function parseInput(data, reportId = 0) {
  if (reportId !== 0) return null;
  const bytes = featureBytes(data);
  if (bytes[0] !== 1 || bytes[1] !== 0) return null;
  if (bytes[2] === 3 && bytes[3] >= 1 && STATE_NAMES[bytes[4]]) return { type: "state", state: bytes[4] };
  if (bytes[2] === 1 && bytes[3] === 60) {
    return { type: "input", sequence: new DataView(bytes.buffer).getUint32(4, true), hex: hex(bytes) };
  }
  return { type: "other", kind: bytes[2], hex: hex(bytes) };
}

export function counterDelta(value, previous) {
  if (previous === undefined || previous === null) return null;
  if (value >= previous) return value - previous;
  if (previous >= 0xf0000000 && value <= 0x0fffffff) return (value - previous) >>> 0;
  return null;
}

export function diagnosticHints(current, previous) {
  if (!current) return ["连接接收器后，诊断计数会在这里更新。"];
  const delta = (key) => previous ? counterDelta(current.counters[key], previous.counters[key]) ?? 0 : current.counters[key];
  const hints = [];
  const link = current.pairingLink;
  const reconnect = current.reconnect;
  if (reconnect?.counters.restore_requests > 0 && reconnect.bondCount === 0) hints.push("正在收到 H=2 重连，但绑定库为空；需先新配对并完成最终密钥保存。");
  if (reconnect?.bootstrapRecords > 0) hints.push("发现 bootstrap 引导密钥记录；旧版可能提前保存。若恢复认证失败，需要重新配对一次以保存最终密钥。");
  if (reconnect?.bondCount === 20 && reconnect.pendingMask && reconnect.counters.commit_failures) hints.push("绑定库已达 20 条且有待保存连接；请先备份并整理记录，固件不会覆盖其他手柄的绑定。");
  if (reconnect?.lastOutcome === 5 && reconnect.bondCount > 0) hints.push("本次恢复请求无法用已保存记录通过 MIC；需检查最终密钥或挑战上下文，不能仅凭 ACK 完成判定已重连。");
  if (reconnect && (reconnect.commitRequestedMask & reconnect.keyNotReadyMask)) hints.push("保存请求已暂存，正在等待手柄以最终密钥完成认证；此时请保持手柄开启。");
  if (link?.lastRx?.ackRequested && link.ackScheduled === 0) hints.push("收到了请求 ACK 的包，但尚未安排 ACK；请核对正在运行的修复固件及底层状态。");
  if (link?.ackScheduled > 0 && link.ackReady === 0) hints.push("ACK 已安排，但没有 TX READY 记录；需要检查 PPI / 定时器启动链。");
  if (link?.ackReady > 0 && link.ackCompleted === 0) hints.push("已有 TX READY，但还没有 ACK 发送完成记录。");
  if (link && link.pipeRx[0] > 0 && link.pipeRx[1] === 0 && !current.counters.authenticated) {
    hints.push("仍停在 pipe0 发现阶段：手柄尚未进入 pipe1 接入。ACK 完成发射不等于手柄已接收，请保留分 pipe / ACK 计数复测。");
  }
  if (delta("storage_failures")) hints.push("Flash 写入失败增加：绑定可能仍未保存，请查看待保存槽。");
  if (delta("late_ack")) hints.push("ACK 逾期增加：需要检查实物中断负载和 ACK 时序。");
  if (delta("mic_failures")) hints.push("MIC 失败增加：检查配对挑战、迁移身份、密钥或会话同步。");
  if (delta("radio_queue_full") + delta("queue_full") + delta("usb_command_drops")) hints.push("队列丢包增加：请减少并发诊断/输入负载。");
  if (!hints.length && delta("authenticated")) hints.push("已有通过认证的手柄上行；可在槽位中检查 USB 输入活动。");
  if (!hints.length && delta("crc_ok")) hints.push("已收到 CRC 正确的射频包，正在等待可认证的手柄业务。");
  if (!hints.length) hints.push("当前没有新的有效射频活动；已有绑定时直接按 Steam 开机，首次或重新配对才使用 X + Steam。");
  return hints;
}

const TRANSFER_ERRORS = { 1: "迁移参数无效", 2: "备份/导入事务已失效，请重试", 3: "请先关闭已连接的手柄，再导入绑定",
  4: "另一个导入事务进行中，请稍后重试", 5: "导入记录尚未完整传输", 6: "绑定写入 Flash 失败，原数据仍保留", 7: "导入记录存在重复手柄标识" };
const tokenBytes = (token) => {
  const bytes = new Uint8Array(4);
  new DataView(bytes.buffer).setUint32(0, token, true);
  return bytes;
};
const tokenAt = (reply, offset) => new DataView(reply.buffer, reply.byteOffset, 64).getUint32(offset, true);

async function transfer(channel, command, args = [], size = 1) {
  const reply = await channel.exchange(command, args, (bytes) => bytes[1] >= 1);
  if (reply[2] !== 0) throw new ProtocolError(TRANSFER_ERRORS[reply[2]] ?? `迁移失败：状态 ${reply[2]}`);
  if (reply[1] < size) throw new ProtocolError("迁移回复被截断");
  return reply;
}

export function validateBackup(value) {
  if (!value || value.format !== "sc2015-receiver-backup" || value.version !== 1 ||
      !Array.isArray(value.bonds) || value.bonds.length > 20) throw new ProtocolError("不是有效的 SC2015 绑定备份（最多 20 条）");
  const address = unhex(value.receiverAddress, 5);
  if (address[0] & 7) throw new ProtocolError("接收器地址首字节低三位必须为零");
  const seen = new Set();
  const bonds = value.bonds.map((bond) => {
    if (!bond || typeof bond !== "object") throw new ProtocolError("绑定条目无效");
    const controllerId = hex(unhex(bond.controllerId, 8));
    const key = hex(unhex(bond.key, 16));
    if (seen.has(controllerId)) throw new ProtocolError("备份中有重复手柄标识");
    seen.add(controllerId);
    return { controllerId, key };
  });
  return { format: "sc2015-receiver-backup", version: 1, receiverAddress: hex(address), bonds };
}

export async function exportBackup(channel) {
  const start = await transfer(channel, COMMAND.SNAPSHOT, [], 13);
  const token = tokenAt(start, 4);
  if (!token || start[3] !== 1 || start[14] !== 20 || start[13] > 20) throw new ProtocolError("不支持的备份协议");
  try {
    const bonds = [];
    for (let index = 0; index < 20; ++index) {
      const reply = await transfer(channel, COMMAND.RECORD, [...tokenBytes(token), index], 31);
      if (reply[3] !== index || reply[4] > 1 || tokenAt(reply, 29) !== token) throw new ProtocolError("备份记录与事务不匹配");
      if (reply[4]) bonds.push({ controllerId: hex(reply.subarray(5, 13)), key: hex(reply.subarray(13, 29)) });
    }
    if (bonds.length !== start[13]) throw new ProtocolError("绑定快照条目数不一致");
    return validateBackup({ format: "sc2015-receiver-backup", version: 1, receiverAddress: hex(start.subarray(8, 13)), bonds });
  } finally {
    await transfer(channel, COMMAND.RELEASE, tokenBytes(token), 5).catch(() => {});
  }
}

export async function importBackup(channel, value, onProgress = () => {}) {
  const backup = validateBackup(value);
  const start = await transfer(channel, COMMAND.STAGE, [...unhex(backup.receiverAddress, 5), backup.bonds.length], 13);
  const token = tokenAt(start, 4);
  if (!token || start[3] !== 1 || start[14] !== 20 || start[13] !== backup.bonds.length ||
      hex(start.subarray(8, 13)) !== backup.receiverAddress) throw new ProtocolError("不支持的导入协议或暂存身份不一致");
  let applied = false;
  try {
    for (let index = 0; index < backup.bonds.length; ++index) {
      const bond = backup.bonds[index];
      const reply = await transfer(channel, COMMAND.WRITE, [...tokenBytes(token), index,
        ...unhex(bond.controllerId, 8), ...unhex(bond.key, 16)], 6);
      if (reply[3] !== index || tokenAt(reply, 4) !== token) throw new ProtocolError("写入记录与导入事务不匹配");
      onProgress(index + 1, backup.bonds.length);
    }
    const reply = await transfer(channel, COMMAND.APPLY, tokenBytes(token), 5);
    if (tokenAt(reply, 3) !== token) throw new ProtocolError("提交事务编号不一致");
    applied = true;
  } finally {
    if (!applied) await transfer(channel, COMMAND.RELEASE, tokenBytes(token), 5).catch(() => {});
  }
  const readback = await exportBackup(channel);
  const canonical = (data) => JSON.stringify({ ...data, bonds: [...data.bonds].sort((a, b) => a.controllerId.localeCompare(b.controllerId)) });
  if (canonical(readback) !== canonical(backup)) throw new ProtocolError("Flash 提交后的绑定回读不一致");
  return readback;
}

export function parseJournalDump(data, receiverAddress) {
  const bytes = data instanceof Uint8Array ? data : new Uint8Array(data);
  let pageA, pageB;
  if (bytes.length === 0x400) { pageA = bytes.subarray(0, 512); pageB = bytes.subarray(512); }
  else if (bytes.length === 0x8000) { pageA = bytes.subarray(0x7400, 0x7600); pageB = bytes.subarray(0x7600, 0x7800); }
  else throw new ProtocolError("需要 32 KiB 完整 MainBlock 或 1024 字节绑定页转储；应用 d0ggle.bin 不含绑定页");
  const erased = (record) => record.every((value) => value === 255);
  // D:44d2 chooses by the complete first 25-byte record in page 7600.
  // Lookup D:4472 scans all twenty records backwards, even across holes.
  const activePage = erased(pageB.subarray(0, 25)) ? 0x7400 : 0x7600;
  const page = activePage === 0x7400 ? pageA : pageB;
  const records = new Map();
  const pages = [pageA, pageB].map((current, pageIndex) => {
    let valid = 0, empty = 0, abnormal = 0;
    for (let index = 0; index < 20; ++index) {
      const record = current.subarray(index * 25, index * 25 + 25);
      if (erased(record)) ++empty;
      else if (record[0] === 0xf0) ++valid;
      else ++abnormal;
    }
    return { address: pageIndex ? 0x7600 : 0x7400, valid, empty, abnormal };
  });
  let superseded = 0;
  for (let index = 19; index >= 0; --index) {
    const record = page.subarray(index * 25, index * 25 + 25);
    if (record[0] !== 0xf0) continue;
    const controllerId = hex(record.subarray(1, 9));
    if (records.has(controllerId)) { ++superseded; continue; }
    records.set(controllerId, { controllerId, key: hex(record.subarray(9, 25)) });
  }
  return { backup: validateBackup({ format: "sc2015-receiver-backup", version: 1,
    receiverAddress, bonds: [...records.values()] }), activePage, pages, superseded,
    integrity: "原 journal 没有 CRC；按启动页选择与 F0 查询标志解析，不代表转储完整性已验证。" };
}
