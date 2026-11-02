// Synthetic USB peer shared by Node and browser tests. No physical HID access.
export function bytesOf(text) {
  return Uint8Array.from(text.match(/../g), (value) => Number.parseInt(value, 16));
}

export function hexOf(bytes) {
  return Array.from(bytes, (value) => value.toString(16).padStart(2, "0")).join("");
}

function put32(bytes, offset, value) { new DataView(bytes.buffer).setUint32(offset, value, true); }
function get32(bytes, offset) { return new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getUint32(offset, true); }

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
    this.devices = Array.from({ length: 4 }, (_, slot) => new FakeDevice(this, slot));
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
    this.reply = new Uint8Array(64);
    this.openCount = this.closeCount = 0;
    this.overrides = [];
  }
  async open() { this.opened = true; ++this.openCount; }
  async close() { this.opened = false; ++this.closeCount; }
  async sendFeatureReport(reportId, bytes) {
    if (!this.opened || reportId !== 0 || bytes.length !== 64) throw new Error("Invalid WebHID transfer");
    this.receiver.calls.push({ slot: this.slot, command: bytes[0], payload: [...bytes] });
    this.reply = this.receiver.report(this.slot, bytes[0], bytes.subarray(2, bytes[1] + 2));
  }
  async receiveFeatureReport(reportId) {
    if (!this.opened || reportId !== 0) throw new Error("Device disconnected");
    const bytes = this.overrides.shift() ?? this.reply;
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
  async requestDevice(options) { this.filters = options.filters; return this.choice; }
  async getDevices() { return this.devices; }
  emit(type, device) { const event = new Event(type); event.device = device; this.dispatchEvent(event); }
}
