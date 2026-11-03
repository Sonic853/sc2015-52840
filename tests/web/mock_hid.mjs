// Synthetic USB peer shared by Node and browser tests. No physical HID access.
export function bytesOf(text) {
  return Uint8Array.from(text.match(/../g), (value) => Number.parseInt(value, 16));
}

export function hexOf(bytes) {
  return Array.from(bytes, (value) => value.toString(16).padStart(2, "0")).join("");
}

function put32(bytes, offset, value) { new DataView(bytes.buffer).setUint32(offset, value, true); }
function get32(bytes, offset) { return new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getUint32(offset, true); }
function mappingCrc(bytes) {
  let value = 0xffffffff;
  for (const byte of bytes) {
    value ^= byte;
    for (let i = 0; i < 8; i++) value = (value >>> 1) ^ ((value & 1) ? 0xedb88320 : 0);
  }
  return (value ^ 0xffffffff) >>> 0;
}

export class FakeReceiver {
  constructor({ address = "2836576294", original = false, version = 2, revision = 3, bonds = [] } = {}) {
    this.address = address;
    this.original = original;
    this.version = version;
    this.revision = revision;
    this.bonds = structuredClone(bonds);
    this.states = [1, 1, 1, 1];
    this.pairing = false;
    this.failCommit = false;
    this.token = 0;
    this.transfer = null;
    this.calls = [];
    this.counters = Array(13).fill(0);
    this.pairingStats = {};
    this.reconnectStats = {};
    this.deferCommit = false;
    this.pendingCommits = new Map();
    this.bootloader = false;
    this.hostStats = {};
    this.transportStats = {};
    this.queueStats = { slots: [], events: [] };
    this.queueId = 0;
    this.queueSnapshot = null;
    this.inputMode = 0;
    this.lastMode = 0; this.bootOnce = 255; this.reboots = 0; this.modeSaves = 0;
    this.failModeSave = false; this.switchSaveFailed = false;
    this.xinputOpened = false;
    this.xinputSubmitted = 0;
    this.xinputCompleted = 0;
    this.mapping = new Uint8Array([1,4,1,2,3,4,5,6,7,8,9,11,10,10,0,11,16,17,184,11,172,13,64,31]);
    if (this.revision >= 16) this.mapping[1] = 0x64;
    this.failConfigSave = false;
    this.configSaves = 0;
    this.devices = Array.from({ length: 4 }, (_, slot) => new FakeDevice(this, slot));
  }
  powerCycle() {
    this.inputMode = this.bootOnce <= 1 ? this.bootOnce : (this.mapping[1] & 8 ? this.lastMode : 0);
    this.bootOnce = 255; ++this.reboots; this.states.fill(1);
    for (const device of this.devices) {
      device.present = this.inputMode === 0;
      device.vendorId = this.inputMode ? 0x045e : 0x28de;
      device.productId = this.inputMode ? 0x028e : 0x1142;
      device.collections = this.inputMode ? [] : [{ usagePage: 0xff00, usage: 1 }];
    }
  }
  restartMode(mode) {
    ++this.modeSaves;
    if (this.failModeSave) { this.switchSaveFailed = true; return; }
    this.switchSaveFailed = false; this.lastMode = mode;
    this.bootOnce = this.mapping[1] & 8 ? 255 : mode;
    this.powerCycle();
  }
  report(slot, command, args) {
    const out = new Uint8Array(64);
    out[0] = command;
    if (command === 0xa1) {
      out[1] = 34;
      out.set([0x55, 0xf7, 0x16, 0x0e], 2);
      out.set(new TextEncoder().encode(this.address.toUpperCase()), 6);
      out.set(new TextEncoder().encode(this.original ? "Valve original" : "SC2015 XIAO"), 16);
    } else if (command === 0xf0) {
      if (this.original) throw new Error("Original donor must never receive F0");
      out[1] = this.version >= 2 ? 62 : 57;
      out[2] = this.version;
      out[3] = (this.pairing ? 3 : 2) | this.states.reduce((mask, state, index) => mask | (state > 1 ? 1 << (index + 2) : 0), 0);
      out[4] = Number(this.pairing);
      const offsets = [5, 9, 13, 17, 21, 25, 29, 35, 39, 43, 47, 51, 55];
      offsets.forEach((offset, index) => put32(out, offset, this.counters[index]));
      out[33] = slot;
      out[34] = 4;
      out.set(bytesOf(this.address), 59);
    } else if (command === 0xf7) {
      const link = this.pairingStats;
      out[1] = 62; out[2] = 1; out[3] = out[63] = this.revision;
      out[4] = link.pipe ?? 0; out[5] = link.channel ?? 40; out[6] = link.length ?? 0;
      out[7] = link.s1 ?? 0; out[8] = link.header ?? 0; out[9] = link.ackLength ?? 0;
      (link.pipeRx ?? [0, 0, 0, 0, 0, 0]).forEach((value, index) => put32(out, 12 + 4 * index, value));
      for (const [key, offset] of [["scheduled", 36], ["ready", 40], ["completed", 44], ["skipped", 48], ["empty", 52]]) {
        put32(out, offset, link[key] ?? 0);
      }
      new DataView(out.buffer).setUint16(56, link.irqDelay ?? 0, true);
      new DataView(out.buffer).setUint16(58, link.readyDelay ?? 0, true);
      out[61] = link.channel ?? 40;
    } else if (command === 0xf8) {
      const data = this.reconnectStats;
      out[1] = 62; out[2] = 1; out[3] = out[63] = this.revision;
      out[4] = data.bondCount ?? this.bonds.length;
      out[5] = data.bootstrapRecords ?? 0;
      out[6] = Number(data.lookupValid ?? false);
      out[7] = data.lastOutcome ?? 0;
      const names = ["lookup_hits", "lookup_misses", "restore_requests", "restore_accepted", "restore_mic_failures",
        "new_pairs", "key_updates", "commit_requests", "commit_deferred", "commit_successes", "commit_failures", "unsaved_disconnects", "restore_fallbacks"];
      names.forEach((name, index) => put32(out, 8 + 4 * index, data[name] ?? 0));
      out[60] = data.pendingMask ?? 0;
      out[61] = data.keyNotReadyMask ?? 0;
      out[62] = data.commitRequestedMask ?? [...this.pendingCommits.keys()].reduce((mask, index) => mask | (1 << index), 0);
    } else if (command === 0xfa) {
      const data = this.hostStats;
      out[1] = 62; out[2] = 1; out[3] = out[63] = this.revision;
      out[4] = data.setSlot ?? 0; out[5] = data.setCommand ?? 0x83; out[6] = 64;
      out[7] = data.getSlot ?? 0; out[8] = data.getCommand ?? 0xae; out[9] = data.replyLength ?? 12;
      if (this.revision >= 9 && data.rejectedReason) {
        out[10] = data.rejectedCommand ?? 0; out[11] = data.rejectedLength ?? 0;
        out[62] = ((data.rejectedSlot ?? 0) << 6) | data.rejectedReason;
      }
      ["sets", "query_busy", "query_ready", "gets", "unpublished_gets", "invalid_sets", "command_drops"].forEach((name, index) => put32(out, 12 + 4 * index, data[name] ?? 0));
      out[60] = this.states.reduce((mask, state, index) => mask | (state > 1 ? 1 << index : 0), 0);
    } else if (command === 0xfb) {
      if (this.original || this.revision < 10) throw new Error("FB requires XIAO revision10");
      const data = this.transportStats;
      out[1] = 62; out[2] = 1; out[3] = out[63] = this.revision;
      ["watchdog_disconnects", "host_disconnects", "replaced_disconnects", "crypto_disconnects",
        "erase_disconnects", "ack_deferred", "stale_session_sets"].forEach((name, i) => put32(out, 4 + 4 * i, data[name] ?? 0));
      for (let slot = 0; slot < 4; slot++) out.set(data.slots?.[slot] ?? [0, 0, 0, 0, 0, 0], 32 + 7 * slot);
      out[60] = this.states.reduce((mask, state, i) => mask | (state > 1 ? 1 << i : 0), 0);
    } else if (command === 0xfc) {
      if (this.original || this.revision < 11) throw new Error("FC requires XIAO revision11");
      const page = args.length ? args[0] : 0;
      let status = 0;
      if (page > 17 || (page === 0 ? args.length > 1 : args.length !== 5)) status = 1;
      else if (page === 0) {
        this.queueId = (this.queueId + 1) >>> 0 || 1;
        this.queueSnapshot = { ...structuredClone(this.queueStats), id: this.queueId,
          clockUs: this.queueStats.clockUs ?? this.queueId * 1000 };
      } else if (!this.queueSnapshot || get32(args, 1) !== this.queueSnapshot.id) status = 2;
      const data = this.queueSnapshot;
      out.set([0xfc, 62, 1, page, this.revision, status]);
      put32(out, 6, data?.id ?? 0); put32(out, 10, data?.clockUs ?? 0);
      if (status) return out;
      const slots = Array.from({ length: 4 }, (_, i) => data.slots?.[i] ?? {});
      const events = (data.events ?? []).slice(-16);
      const sum = (name) => slots.reduce((total, s) => total + (s[name] ?? 0), 0);
      const max = (name) => Math.max(...slots.map((s) => s[name] ?? 0));
      if (page === 0) {
        out.set([12, 31, events.length, data.usbHighWater ?? 0], 14);
        [sum("haptic_admitted"), sum("haptic_full"), [2, 3, 4, 5, 6].reduce((total, i) => total + sum(`a${i}_full`), 0),
          sum("initial_retries") + sum("count_retries"), sum("usb_haptic_waits"), sum("usb_haptic_timeouts"),
          max("max_fifo_age_us"), max("usb_max_age_us"), data.maxRxLatencyUs ?? 0, data.maxProcessGapUs ?? 0]
          .forEach((value, i) => put32(out, 18 + 4 * i, value));
        put32(out, 58, 8000); out[62] = max("fifoHighWater"); out[63] = Number(data.clockAvailable ?? true);
      } else if (page <= 8) {
        const s = slots[(page - 1) % 4];
        const names = page <= 4 ? ["admitted", "haptic_admitted", "haptic_full", "a2_full", "a3_full", "a4_full", "a5_full",
          "a6_full", "other_full", "initial_retries", "count_retries", "commit_waits"] :
          ["offline", "serialized", "retired", "cleared_fragments", "max_fifo_age_us", "max_lane_age_us", "usb_admitted",
            "usb_processed", "usb_haptic_received", "usb_haptic_waits", "usb_haptic_timeouts", "usb_haptic_stale"];
        names.forEach((name, i) => put32(out, 14 + 4 * i, s[name] ?? 0));
        out[62] = s.fifoHighWater ?? 0; out[63] = s.pendingHaptics ?? 0;
      } else if (page <= 16) {
        const rows = events.slice((page - 9) * 2, (page - 9) * 2 + 2);
        rows.forEach((event, i) => {
          const offset = 14 + 24 * i;
          ["id", "at_us", "generation", "age_us"].forEach((name, j) => put32(out, offset + 4 * j, event[name] ?? 0));
          out.set(["slot", "kind", "command", "length", "needed", "fifo_count", "lane_flags", "usb_depth"]
            .map((name) => event[name] ?? 0), offset + 16);
        });
        out[62] = rows.length;
      } else {
        slots.forEach((s, i) => ["usb_max_age_us", "usb_haptic_offline", "usb_haptic_reset_cancelled"].forEach((name, j) =>
          put32(out, 14 + 12 * i + 4 * j, s[name] ?? 0)));
        out[62] = data.usbDepth ?? 0; out[63] = 4;
      }
    } else if (command === 0xfd) {
      if (this.original || this.revision < 12) throw new Error("FD requires XIAO revision12");
      out.set([0xfd,62,1,this.revision,0,this.inputMode,this.inputMode,255,5]);
      if (this.revision >= 16) out[8] = 1 | (this.mapping[1] & 8 ? 8 : 4) | (this.switchSaveFailed ? 16 : 0);
      if (this.revision >= 14) {
        out[9] = 1; out[10] = 4 | this.inputMode | (this.xinputOpened ? 2 : 0);
        const view = new DataView(out.buffer);
        view.setUint32(28, this.xinputSubmitted, true); view.setUint32(32, this.xinputCompleted, true);
      }
      if (args.length) {
        if (args.length !== 5 || hexOf(args.subarray(0, 4)) !== "4d4f4445" || args[4] > 1) out[4] = 1;
        else if (this.transfer?.type === "import") out[4] = 4;
        else out[6] = args[4];
      }
    } else if (command === 0xfe) {
      if (this.original || this.revision < 12) throw new Error("FE requires XIAO revision12");
      out.set([0xfe,62,1,this.revision,0,24]);
      if (args.length) {
        if (args.length !== 29 || args[0] !== 1) out[4] = 1;
        else if (get32(args, 1) !== mappingCrc(this.mapping)) out[4] = 2;
        else if (this.failConfigSave) out[4] = 6;
        else { this.mapping = args.slice(5); ++this.configSaves; }
      }
      put32(out, 8, mappingCrc(this.mapping)); out.set(this.mapping, 12);
    } else if (command === 0xf9) {
      if (this.original || this.revision < 5) throw new Error("F9 requires XIAO revision5");
      out[1] = 2; out[3] = 1;
      out[2] = hexOf(args) !== "55463221" ? 1 : this.transfer?.type === "import" ? 4 : 0;
    } else if (command === 0xb4) {
      if (this.pendingCommits.has(slot)) {
        const reads = this.pendingCommits.get(slot) + 1;
        this.pendingCommits.set(slot, reads);
        if (reads >= 2) { this.states[slot] = 2; this.pendingCommits.delete(slot); }
      }
      out[1] = 1;
      out[2] = this.states[slot];
    } else if (command === 0xad) this.pairing = Boolean(args[0]);
    else if (command === 0xb3 && this.states[slot] === 3) {
      if (this.failCommit) ++this.counters[6];
      else if (this.deferCommit) this.pendingCommits.set(slot, 0);
      else this.states[slot] = 2;
    } else if (command === 0xb2) this.states[slot] = 1;
    else if (command >= 0xf1 && command <= 0xf6) {
      out[1] = 1;
      const status = (value) => { out[2] = value; return out; };
      if (command === 0xf1 || command === 0xf3) {
        if (this.transfer?.type === "import") return status(4);
        if (command === 0xf3 && this.states.some((state) => state > 1)) return status(3);
        this.transfer = { token: ++this.token, type: command === 0xf1 ? "export" : "import",
          address: command === 0xf1 ? this.address : hexOf(args.subarray(0, 5)),
          bonds: command === 0xf1 ? structuredClone(this.bonds) : [], count: command === 0xf1 ? this.bonds.length : args[5] };
        out[1] = 13;
        out[3] = 1;
        put32(out, 4, this.token);
        out.set(bytesOf(this.transfer.address), 8);
        out[13] = this.transfer.count;
        out[14] = 20;
      } else {
        const token = get32(args, 0);
        if (!this.transfer || token !== this.transfer.token) return status(2);
        if (command === 0xf6) { this.transfer = null; out[1] = 5; put32(out, 3, token); }
        else if (command === 0xf2) {
          const index = args[4];
          out[1] = 31;
          out[3] = index;
          const bond = this.transfer.bonds[index];
          if (bond) { out[4] = 1; out.set(bytesOf(bond.controllerId), 5); out.set(bytesOf(bond.key), 13); }
          put32(out, 29, token);
        } else if (command === 0xf4) {
          const index = args[4];
          this.transfer.bonds[index] = { controllerId: hexOf(args.subarray(5, 13)), key: hexOf(args.subarray(13, 29)) };
          out[1] = 6;
          out[3] = index;
          put32(out, 4, token);
        } else if (command === 0xf5) {
          if (this.failCommit) return status(6);
          if (this.transfer.bonds.filter(Boolean).length !== this.transfer.count) return status(5);
          this.address = this.transfer.address;
          this.bonds = structuredClone(this.transfer.bonds);
          this.transfer = null;
          out[1] = 5;
          put32(out, 3, token);
        }
      }
    }
    return out;
  }
  input(slot, state) {
    this.states[slot] = state;
    const bytes = new Uint8Array(64);
    bytes.set([1, 0, 3, 1, state]);
    this.devices[slot].emitInput(bytes);
  }
}

export class FakeDevice extends EventTarget {
  constructor(receiver, slot) {
    super();
    this.receiver = receiver;
    this.slot = slot;
    this.vendorId = 0x28de;
    this.productId = 0x1142;
    this.productName = "Steam Controller";
    this.collections = [{ usagePage: 0xff00, usage: 1 }];
    this.opened = false;
    this.present = true;
    this.reply = new Uint8Array(64);
    this.openCount = this.closeCount = 0;
    this.overrides = [];
  }
  async open() { if (!this.present) throw new Error("No configuration HID in Xbox mode"); this.opened = true; ++this.openCount; }
  async close() { this.opened = false; ++this.closeCount; }
  async sendFeatureReport(reportId, bytes) {
    if (!this.opened || reportId !== 0 || bytes.length !== 64) throw new Error("Invalid WebHID transfer");
    this.receiver.calls.push({ slot: this.slot, command: bytes[0], payload: [...bytes] });
    this.reply = this.receiver.report(this.slot, bytes[0], bytes.subarray(2, bytes[1] + 2));
  }
  async receiveFeatureReport(reportId) {
    if (!this.opened || reportId !== 0) throw new Error("Device disconnected");
    const bytes = this.overrides.shift() ?? this.reply;
    if (bytes[0] === 0xf9 && bytes[1] === 2 && bytes[2] === 0) this.receiver.bootloader = true;
    if (bytes[0] === 0xfd && bytes[4] === 0 && bytes[5] !== bytes[6]) {
      if (this.receiver.revision >= 16) this.receiver.restartMode(bytes[6]);
      else {
        this.receiver.inputMode = bytes[6];
        for (const device of this.receiver.devices) device.productId = bytes[6] ? 0x1143 : 0x1142;
      }
    }
    // A nonzero offset catches DataView consumers accidentally parsing the
    // underlying buffer instead of the returned report slice.
    const allocation = new Uint8Array(72);
    allocation.set(bytes, 4);
    return new DataView(allocation.buffer, 4, 64);
  }
  emitInput(bytes, reportId = 0) {
    const event = new Event("inputreport");
    event.data = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    event.reportId = reportId;
    this.dispatchEvent(event);
  }
}

export class FakeHid extends EventTarget {
  constructor(devices) { super(); this.devices = devices; this.choice = devices; this.filters = null; }
  async requestDevice(options) { this.filters = options.filters; return this.choice.filter((d) => d.present !== false); }
  async getDevices() { return this.devices.filter((d) => d.present !== false); }
  emit(type, device) { const event = new Event(type); event.device = device; this.dispatchEvent(event); }
}
