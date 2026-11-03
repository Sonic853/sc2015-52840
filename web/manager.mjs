import { FILTERS, COMMAND, FeatureChannel, FeatureReplyError, ProtocolError, isReceiver, parseInput,
  readIdentity, readDiagnostics, readQueueDiagnostics, QueueSnapshotExpiredError, readState, exportBackup, importBackup,
  readInputMode, switchInputMode, readInputMapping, saveInputMapping, INPUT_MODES } from "./protocol.mjs";

export class ReceiverManager extends EventTarget {
  constructor(hid, channelOptions = {}) {
    super();
    this.hid = hid;
    this.channelOptions = channelOptions;
    this.wait = channelOptions.wait ?? ((ms) => new Promise((resolve) => setTimeout(resolve, ms)));
    this.entries = new Map();
    this.queueSnapshots = new Map();
    this.inputMappings = new Map();
    this.armed = new Map();
    this.active = false;
    this.generation = 0;
    this.tail = Promise.resolve();
    this.onDisconnect = (event) => this.remove(event.device, "USB 接收器已拔出");
    this.onConnect = (event) => {
      if (this.active) this.enqueue(() => this.attach([event.device])).catch((error) => this.log(error.message, "error"));
    };
    hid?.addEventListener("disconnect", this.onDisconnect);
    hid?.addEventListener("connect", this.onConnect);
  }
  notify() { this.dispatchEvent(new Event("change")); }
  log(message, level = "info") { this.dispatchEvent(new CustomEvent("log", { detail: { message, level, time: Date.now() } })); }
  enqueue(operation) {
    const result = this.tail.then(operation);
    this.tail = result.catch(() => {});
    return result;
  }
  groups() {
    const result = new Map();
    for (const entry of this.entries.values()) {
      const id = entry.info.receiverId;
      const key = `${entry.kind}:${id}`;
      if (!result.has(key)) result.set(key, { id, key, kind: entry.kind, entries: [], diagnostic: null, previous: null,
        queueSnapshot: entry.kind === "xiao" ? this.queueSnapshots.get(id) ?? null : null,
        inputMapping: entry.kind === "xiao" ? this.inputMappings.get(id) ?? null : null });
      const group = result.get(key);
      group.entries.push(entry);
      if (entry.diagnostic && (!group.diagnostic || entry.diagnostic.timestamp > group.diagnostic.timestamp)) {
        group.diagnostic = entry.diagnostic;
        group.previous = entry.previous;
      }
    }
    for (const group of result.values()) group.entries.sort((a, b) => (a.info.slot ?? 0) - (b.info.slot ?? 0));
    return [...result.values()];
  }
  group(id, xiao = true) {
    const group = this.groups().find((item) => item.id === id && (!xiao || item.kind === "xiao"));
    if (!group) throw new ProtocolError("请先选择已连接的 XIAO 接收器");
    return group;
  }
  async request() {
    if (!this.hid) throw new ProtocolError("此浏览器不支持 WebHID，请使用桌面版 Chrome 或 Edge");
    // requestDevice must start directly inside the click gesture, not behind
    // an awaited background poll or the serialized operation queue.
    const generation = this.generation;
    const devices = await this.hid.requestDevice({ filters: [...FILTERS] });
    if (generation !== this.generation) return 0;
    this.active = true;
    return this.enqueue(() => this.attach(devices));
  }
  async restore() {
    if (!this.hid) throw new ProtocolError("此浏览器不支持 WebHID");
    this.active = true;
    const devices = await this.hid.getDevices();
    return this.enqueue(() => this.attach(devices));
  }
  async attach(devices) {
    if (!this.active) return 0;
    const generation = this.generation;
    let attached = 0;
    const failures = [];
    for (const device of devices) {
      if (!isReceiver(device)) continue;
      const previous = this.entries.get(device);
      if (previous && !previous.channel.closed) continue;
      if (previous) this.remove(device, null);
      const channel = new FeatureChannel(device, this.channelOptions);
      try {
        await channel.open();
        const identity = await readIdentity(channel);
        let diagnostic = null, state = null;
        if (identity.isXiao) {
          diagnostic = await readDiagnostics(channel);
          if (diagnostic.receiverId !== identity.receiverId) throw new ProtocolError("A1 与 F0 的接收器身份不一致");
          if (diagnostic.inputMode && !this.inputMappings.has(identity.receiverId))
            this.inputMappings.set(identity.receiverId, await readInputMapping(channel));
          state = await readState(channel);
        }
        if (generation !== this.generation || !this.active) { await channel.close(); continue; }
        const kind = identity.isXiao ? "xiao" : "original";
        const info = { ...identity, ...(diagnostic ? { slot: diagnostic.slot } : {}) };
        // Multiple logical interfaces on the original dongle share A1 but do
        // not expose a slot number. Keep one identity-only donor handle.
        const existing = [...this.entries.values()].find((entry) => entry.kind === kind &&
          entry.info.receiverId === info.receiverId && entry.info.slot === info.slot);
        if (existing) {
          if (kind === "xiao") throw new ProtocolError("出现重复的接收器身份和槽号；请只连接一只使用该身份的 XIAO");
          await channel.close();
          continue;
        }
        const entry = { device, channel, kind, info, diagnostic, previous: null, state,
          inputCount: 0, lastInputAt: null, lastInput: null, queryError: null, onInput: null };
        entry.onInput = (event) => {
          if (this.entries.get(device) !== entry || kind !== "xiao") return;
          try {
            const report = parseInput(event.data, event.reportId);
            if (report?.type === "state") this.setState(entry, report.state);
            if (report?.type === "input") {
              ++entry.inputCount;
              entry.lastInputAt = Date.now();
              entry.lastInput = report;
            }
            this.notify();
          } catch { /* Non-controller collections cannot update slot state. */ }
        };
        device.addEventListener("inputreport", entry.onInput);
        this.entries.set(device, entry);
        ++attached;
        this.log(kind === "xiao" ? `已连接 ${info.receiverId} · 槽 ${info.slot + 1}` : `已读取原接收器身份 ${info.receiverId}`);
      } catch (error) {
        await channel.close().catch(() => {});
        failures.push(error);
      }
    }
    this.notify();
    if (failures.length) this.log(failures[0].message, "error");
    if (!attached && !this.entries.size && failures.length) throw failures[0];
    return attached;
  }
  setState(entry, state) {
    if (entry.state !== state) {
      const words = { 1: "无线断开", 2: "已连接且绑定已保存", 3: "新绑定等待保存" };
      this.log(`槽 ${entry.info.slot + 1}：${words[state]}`);
      if (state !== 3) this.armed.get(entry.info.receiverId)?.attempted.delete(entry.info.slot);
      entry.state = state;
    }
  }
  remove(device, message, level = "error") {
    const entry = this.entries.get(device);
    if (!entry) return;
    this.entries.delete(device);
    device.removeEventListener("inputreport", entry.onInput);
    entry.channel.close().catch(() => {});
    if (!this.groups().some((group) => group.kind === "xiao" && group.id === entry.info.receiverId)) {
      this.armed.delete(entry.info.receiverId);
      this.queueSnapshots.delete(entry.info.receiverId);
      this.inputMappings.delete(entry.info.receiverId);
    }
    if (message) this.log(message, level);
    this.notify();
  }
  async refresh() {
    for (const group of this.groups().filter((item) => item.kind === "xiao")) {
      let sampled = false, fresh = null;
      const modern = Boolean(group.diagnostic?.reconnect);
      // F0/F8 already contain all slot states. Avoid polling B4: it produces
      // legacy connection notifications and can interrupt Steam initialization.
      const entries = modern ? [...group.entries].sort((a, b) => Number(a.state !== 1) - Number(b.state !== 1)) : group.entries;
      for (const entry of entries) {
        if (this.entries.get(entry.device) !== entry || entry.channel.closed) continue;
        if (modern && sampled) continue;
        try {
          if (!sampled) {
            const diagnostic = await readDiagnostics(entry.channel);
            if (diagnostic.slot !== entry.info.slot || diagnostic.receiverId !== entry.info.receiverId) {
              this.remove(entry.device, "接收器身份已更改，请重新连接网页接口");
              continue;
            }
            entry.previous = entry.diagnostic;
            entry.diagnostic = diagnostic;
            fresh = diagnostic;
            sampled = true;
          }
          if (!modern) this.setState(entry, await readState(entry.channel));
          if (entry.queryError) this.log(`槽 ${entry.info.slot + 1}：Feature 查询已恢复`);
          entry.queryError = null;
        } catch (error) {
          // A missing/mismatched reply is not a wireless disconnect. Preserve
          // input reports and the last known state; quarantine truly hung USB.
          if (!(error instanceof FeatureReplyError) && !entry.channel.closed) await entry.channel.close().catch(() => {});
          const message = entry.channel.closed ? `${error.message}；已暂停此接口查询，请重新连接 / 授权接口` : error.message;
          if (entry.queryError !== message) this.log(`槽 ${entry.info.slot + 1}：${message}`, "error");
          entry.queryError = message;
        }
      }
      if (modern && fresh?.reconnect) {
        for (const entry of group.entries) {
          if (this.entries.get(entry.device) !== entry) continue;
          const bit = 1 << entry.info.slot;
          this.setState(entry, !(fresh.activePipes & (bit << 2)) ? 1 : fresh.reconnect.pendingMask & bit ? 3 : 2);
        }
      }
      const armed = this.armed.get(group.id);
      if (armed?.auto) {
        for (const entry of group.entries) {
          if (this.entries.get(entry.device) !== entry || entry.queryError || entry.channel.closed || entry.state !== 3 ||
              armed.skip.has(entry.info.slot) || armed.attempted.has(entry.info.slot)) continue;
          armed.attempted.add(entry.info.slot);
          try { await this.commit(entry); }
          catch (error) { this.log(error.message, "error"); }
        }
      }
    }
    this.notify();
  }
  // Call through enqueue so a routine page-0 poll cannot expire this read.
  inputEntry(id) {
    const group = this.group(id);
    if ((group.diagnostic?.pairingLink?.firmwareRevision ?? 0) < 12) throw new ProtocolError("输入模式与按键配置需要修订号 12 或更新固件");
    const entry = group.entries.filter((item) => !item.channel.closed && !item.queryError)
      .sort((a, b) => Number(a.state !== 1) - Number(b.state !== 1))[0];
    if (!entry) throw new ProtocolError("没有可用的配置接口，请重新连接 / 授权接口");
    return entry;
  }
  async loadInputMapping(id) {
    const entry = this.inputEntry(id);
    const result = await readInputMapping(entry.channel);
    this.inputMappings.set(id, result); this.notify();
    return result;
  }
  async saveInputMapping(id, value) {
    const entry = this.inputEntry(id);
    const previous = this.inputMappings.get(id);
    const result = await saveInputMapping(entry.channel, previous, value);
    this.inputMappings.set(id, result);
    this.log(`${id} Xbox 按键配置已保存并回读确认`); this.notify();
    return result;
  }
  async switchInputMode(id, mode) {
    const entry = this.inputEntry(id);
    const current = await readInputMode(entry.channel);
    if (current.mode === mode) { this.log(`已经是${INPUT_MODES[mode]}`); return current; }
    const result = await switchInputMode(entry.channel, mode);
    const entries = [...this.group(id).entries];
    for (const item of entries) {
      this.remove(item.device, null);
      await item.channel.close().catch(() => {});
    }
    this.log(result.firmwareRevision >= 16 ? `已确认切换到${INPUT_MODES[mode]}，接收器将保存启动选择并重启；${mode === 2 ? "PS5" : "Xbox"} 无网页接口，A + Steam 开机返回 Steam 配置`
      : `已确认切换到${INPUT_MODES[mode]}，等待 USB 重新枚举；首次请连接 / 授权新身份`);
    return result;
  }

  // Call through enqueue so a routine page-0 poll cannot expire this read.
  async captureQueueSnapshot(id) {
    const group = this.group(id);
    if ((group.diagnostic?.pairingLink?.firmwareRevision ?? 0) < 11) throw new ProtocolError("冻结队列快照需要修订号 11 或更新的 XIAO 固件");
    const entry = group.entries.filter((item) => !item.channel.closed && !item.queryError)
      .sort((a, b) => Number(a.state !== 1) - Number(b.state !== 1))[0];
    if (!entry) throw new ProtocolError("没有可用的健康接口；请刷新或重新连接接收器接口");
    try {
      const snapshot = await readQueueDiagnostics(entry.channel, { detail: true });
      if (this.entries.get(entry.device) !== entry) throw new ProtocolError("队列快照读取期间接口已断开");
      const captured = { ...snapshot, capturedAt: new Date().toISOString() };
      this.queueSnapshots.set(id, captured);
      this.log(`${id} 冻结队列快照 ${snapshot.snapshotId} 已读取 · MCU ${snapshot.snapshotClockUs}µs`);
      this.notify();
      return captured;
    } catch (error) {
      if (!(error instanceof FeatureReplyError) && !(error instanceof QueueSnapshotExpiredError) && !entry.channel.closed)
        await entry.channel.close().catch(() => {});
      entry.queryError = entry.channel.closed ? `${error.message}；已暂停此接口查询，请重新连接 / 授权接口` : error.message;
      this.log(`槽 ${entry.info.slot + 1}：${entry.queryError}`, "error");
      this.notify();
      throw error;
    }
  }
  async pairing(id, enabled, auto = false) {
    const group = this.group(id);
    const entry = group.entries[0];
    this.armed.delete(id);
    // Take a current baseline, not a possibly old display snapshot. Pending
    // bonds that appeared while polling was paused remain manual commits.
    if (enabled) {
      for (const item of group.entries) this.setState(item, await readState(item.channel));
    }
    const skipped = new Set(group.entries.filter((item) => item.state === 3).map((item) => item.info.slot));
    await entry.channel.exchange(COMMAND.PAIR, [enabled ? 1 : 0, 0]);
    entry.previous = entry.diagnostic;
    entry.diagnostic = await readDiagnostics(entry.channel);
    if (entry.diagnostic.pairing !== enabled) throw new ProtocolError(enabled ? "接收器未开启配对；请检查是否已满四个连接槽" : "关闭配对尚未生效");
    if (enabled) this.armed.set(id, { auto, skip: skipped, attempted: new Set() });
    this.log(enabled ? "已开启配对，请按住 X + Steam 启动 2015 版手柄" : "已关闭新配对");
    this.notify();
  }
  async commit(entry) {
    if (!entry || this.entries.get(entry.device) !== entry) throw new ProtocolError("槽接口已断开");
    if (entry.state !== 3) throw new ProtocolError("此槽没有待保存的新绑定");
    const previousFailures = entry.diagnostic?.counters.storage_failures ?? 0;
    await entry.channel.exchange(COMMAND.COMMIT);
    for (let attempt = 0; attempt < 24; ++attempt) {
      this.setState(entry, await readState(entry.channel));
      if (entry.state === 2) break;
      if (entry.state === 1) throw new ProtocolError("保存尚未完成，手柄已断开；请重新配对并保持开启");
      entry.previous = entry.diagnostic;
      entry.diagnostic = await readDiagnostics(entry.channel);
      if (entry.diagnostic.counters.storage_failures > previousFailures) throw new ProtocolError("绑定写入 Flash 失败，仍为待保存状态");
      this.notify();
      await this.wait(40);
    }
    if (entry.state !== 2) throw new ProtocolError("保存请求仍在等待最终密钥认证；请保持手柄开启并查看重连诊断");
    this.log(`槽 ${entry.info.slot + 1} 的绑定已保存`);
    this.notify();
  }
  async disconnect(entry) {
    if (!entry) throw new ProtocolError("槽接口未授权");
    await entry.channel.exchange(COMMAND.DISCONNECT);
    this.setState(entry, await readState(entry.channel));
    this.notify();
  }
  async backup(id) {
    return exportBackup(this.group(id).entries[0].channel);
  }
  async bootloader(id) {
    const group = this.group(id);
    const entry = group.entries.find((item) => !item.channel.closed && !item.queryError) ??
      group.entries.find((item) => !item.channel.closed);
    if (!entry) throw new ProtocolError("请先重新连接接收器接口");
    // Recheck the selected XIAO, so stale display data cannot enable F9 on an
    // old image. Original Valve receivers never pass this path.
    const diagnostic = await readDiagnostics(entry.channel);
    if (diagnostic.receiverId !== id || diagnostic.slot !== entry.info.slot) throw new ProtocolError("接收器身份已更改，请重新连接");
    if ((diagnostic.pairingLink?.firmwareRevision ?? 0) < 5) throw new ProtocolError("网页刷入模式需要修订号 5 或更新固件；首次更新请双击 RESET 进入 UF2 盘");
    const reply = await entry.channel.exchange(COMMAND.BOOTLOADER, [0x55, 0x46, 0x32, 0x21],
      (bytes) => bytes[1] === 2 && bytes[3] === 1);
    if (reply[2] !== 0) throw new ProtocolError(reply[2] === 4 ? "绑定导入尚未结束，请完成或取消后再进入刷入模式" : "接收器拒绝进入刷入模式");
    this.armed.delete(id);
    for (const item of group.entries) this.remove(item.device, null);
    this.log(`${id} 已确认 UF2 刷入模式请求；等待磁盘出现后复制 XIAO UF2，刷入后重新连接网页`);
    this.notify();
  }
  async import(id, backup, onProgress) {
    const group = this.group(id);
    if (this.groups().some((item) => item.kind === "xiao" && item.id !== id && item.id === backup.receiverAddress)) {
      throw new ProtocolError("已有另一只使用目标身份的 XIAO，请先断开该设备");
    }
    this.armed.delete(id);
    const result = await importBackup(group.entries[0].channel, backup, onProgress);
    for (const entry of group.entries) {
      const info = await readDiagnostics(entry.channel);
      entry.info = { ...entry.info, receiverId: info.receiverId, slot: info.slot };
      entry.diagnostic = info;
      entry.previous = null;
      this.setState(entry, await readState(entry.channel));
    }
    this.log(`已应用 ${result.bonds.length} 条绑定并完成回读；接收器身份为 ${result.receiverAddress}`);
    this.notify();
    return result;
  }
  async close() {
    this.active = false;
    ++this.generation;
    this.armed.clear();
    this.queueSnapshots.clear();
    this.inputMappings.clear();
    const entries = [...this.entries.values()];
    this.entries.clear();
    for (const entry of entries) entry.device.removeEventListener("inputreport", entry.onInput);
    await Promise.all(entries.map((entry) => entry.channel.close().catch(() => {})));
    this.notify();
  }
  async destroy() {
    this.hid?.removeEventListener("connect", this.onConnect);
    this.hid?.removeEventListener("disconnect", this.onDisconnect);
    await this.close();
  }
}
