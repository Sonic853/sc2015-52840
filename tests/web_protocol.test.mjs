import test from "node:test";
import assert from "node:assert/strict";
import { FeatureChannel, COMMAND, commandBytes, featureBytes, parseDiagnostics, parseIdentity,
  readDiagnostics, counterDelta, parseInput, exportBackup, importBackup, validateBackup, parseJournalDump,
  diagnosticHints, readQueueDiagnostics, QueueSnapshotExpiredError } from "../web/protocol.mjs";
import { FakeReceiver, bytesOf } from "./web/mock_hid.mjs";

const options = { wait: async () => {}, attempts: 5 };
const bond = { controllerId: "0102030405060708", key: "00112233445566778899aabbccddeeff" };
const backup = { format: "sc2015-receiver-backup", version: 1, receiverAddress: "a812345678", bonds: [bond] };

test("WebHID uses a separate zero report ID, exact 64-byte payload and serial feature operations", async () => {
  const receiver = new FakeReceiver();
  const device = receiver.devices[0];
  const channel = new FeatureChannel(device, options);
  await channel.open();
  const [pair, state] = await Promise.all([
    channel.exchange(COMMAND.PAIR, [1, 0]), channel.exchange(COMMAND.STATE, [], (reply) => reply[1] === 1),
  ]);
  assert.equal(pair[0], 0xad);
  assert.equal(state[2], 1);
  assert.deepEqual(receiver.calls.map((item) => item.command), [0xad, 0xb4]);
  assert.deepEqual(receiver.calls[0].payload.slice(0, 5), [0xad, 2, 1, 0, 0]);
  assert.equal(receiver.calls[0].payload.length, 64);
  await channel.close();
});

test("placeholder and stale command replies are retried, including DataView byte offsets", async () => {
  const receiver = new FakeReceiver();
  const device = receiver.devices[3];
  device.overrides.push(new Uint8Array(64), Uint8Array.from({ length: 64 }, (_, i) => i ? 0 : 0xf0));
  const channel = new FeatureChannel(device, options);
  await channel.open();
  const parsed = await readDiagnostics(channel);
  assert.equal(parsed.slot, 3);
  assert.equal(parsed.receiverId, receiver.address);
  assert.equal(parsed.version, 2);
  const normalized = featureBytes(new DataView(Uint8Array.from([0, ...receiver.report(3, 0xf0, [])]).buffer));
  assert.equal(normalized[33], 3);
  assert.throws(() => featureBytes(new Uint8Array(63)), /64/);
});

test("hung HID requests quarantine the channel instead of overlapping subsequent operations", async () => {
  const receiver = new FakeReceiver();
  const device = receiver.devices[0];
  let sends = 0;
  device.sendFeatureReport = () => { ++sends; return new Promise(() => {}); };
  const channel = new FeatureChannel(device, { ...options, timeout: 20 });
  await channel.open();
  const result = await Promise.allSettled([channel.exchange(0xad, [1, 0]), channel.exchange(0xb4)]);
  assert.equal(sends, 1);
  assert.equal(result.every((item) => item.status === "rejected"), true);
  assert.equal(channel.closed, true);
  assert.equal(device.opened, false);
});

test("legacy query SET busy and unpublished GET stalls retry within the serialized transaction", async () => {
  const receiver = new FakeReceiver();
  const device = receiver.devices[0];
  const send = device.sendFeatureReport.bind(device);
  const receive = device.receiveFeatureReport.bind(device);
  let sets = 0, gets = 0;
  device.sendFeatureReport = async (id, bytes) => {
    if (bytes[0] === 0x83 && ++sets < 3) throw new DOMException("pending radio response", "NetworkError");
    await send(id, bytes);
    if (bytes[0] === 0x83) device.reply.set([0x83, 5, 1, 6, 0x11, 0, 0]);
  };
  device.receiveFeatureReport = async (id) => {
    if (++gets <= 2) throw new DOMException("not yet published", "NetworkError");
    return receive(id);
  };
  const channel = new FeatureChannel(device, options);
  await channel.open();
  const [attributes, state] = await Promise.all([channel.exchange(0x83), channel.exchange(COMMAND.STATE)]);
  assert.equal(sets, 3);
  assert.equal(attributes[1], 5);
  assert.equal(state[2], 1);
  assert.equal(channel.closed, false);
  assert.deepEqual(receiver.calls.map((call) => call.command), [0x83, 0xb4]);
  await channel.close();
});

test("USB write errors are not retried as read-query busy, and permissions are not swallowed", async () => {
  const receiver = new FakeReceiver();
  const device = receiver.devices[0];
  let sends = 0;
  device.sendFeatureReport = async () => { ++sends; throw new DOMException("write failure", "NetworkError"); };
  const channel = new FeatureChannel(device, options);
  await channel.open();
  await assert.rejects(channel.exchange(0xb3), { name: "NetworkError" });
  assert.equal(sends, 1);
  device.sendFeatureReport = async () => { ++sends; throw new DOMException("permission revoked", "NotAllowedError"); };
  await assert.rejects(channel.exchange(0x83), { name: "NotAllowedError" });
  assert.equal(sends, 2);
  await channel.close();
});

test("identity preserves CHIPID order and normalizes only the first byte", () => {
  const receiver = new FakeReceiver({ original: true, address: "ab12345678" });
  const identity = parseIdentity(receiver.report(0, 0xa1, []));
  assert.equal(identity.rawChipId, "ab12345678");
  assert.equal(identity.receiverId, "a812345678");
  assert.equal(identity.isXiao, false);
  assert.throws(() => parseDiagnostics(new FakeReceiver({ version: 1 }).report(0, 0xf0, [])), /v2/);
  const invalid = new FakeReceiver().report(0, 0xf0, []);
  invalid[33] = 4;
  assert.throws(() => parseDiagnostics(invalid), /槽号/);
  assert.throws(() => commandBytes(0xad, new Uint8Array(63)), /参数/);
  assert.throws(() => commandBytes(0xad, [256]), /参数/);
});

test("counter wrap/reset and input report parsing do not invent activity", () => {
  assert.equal(counterDelta(9, 7), 2);
  assert.equal(counterDelta(5, 0xfffffffd), 8);
  assert.equal(counterDelta(1, 100), null);
  const input = new Uint8Array(64);
  input.set([1, 0, 1, 60]);
  new DataView(input.buffer).setUint32(4, 0xfedcba98, true);
  assert.equal(parseInput(new DataView(input.buffer)).sequence, 0xfedcba98);
  assert.equal(parseInput(input, 1), null);
  input[2] = 3; input[3] = 1; input[4] = 3;
  assert.deepEqual(parseInput(input), { type: "state", state: 3 });
});

test("v3 captures actual ACK stages and distinguishes discovery from authentication", async () => {
  const receiver = new FakeReceiver({ version: 3 });
  receiver.counters[0] = receiver.counters[7] = 2076;
  receiver.pairingStats = { pipeRx: [2076, 0, 0, 0, 0, 0], length: 9, s1: 3,
    scheduled: 0, ready: 0, completed: 0, ackLength: 22, irqDelay: 4, readyDelay: 132 };
  const channel = new FeatureChannel(receiver.devices[0], options);
  await channel.open();
  const data = await readDiagnostics(channel);
  assert.equal(data.version, 3);
  assert.equal(data.pairingLink.firmwareRevision, 3);
  assert.equal(data.pairingLink.lastRx.ackRequested, true);
  assert.equal(data.pairingLink.pipeRx[0], 2076);
  assert.equal(data.pairingLink.lastTxReadyDelayUs, 132);
  assert.equal(diagnosticHints(data).some((hint) => hint.includes("尚未安排 ACK")), true);
  assert.equal(diagnosticHints(data).some((hint) => hint.includes("pipe0")), true);
  assert.deepEqual(receiver.calls.map((item) => item.command), [0xf0, 0xf7]);
});

test("revision4 exposes non-secret persisted bond, restore selector and deferred commit diagnosis", async () => {
  const receiver = new FakeReceiver({ version: 3, revision: 4 });
  receiver.reconnectStats = { bondCount: 0, lastOutcome: 4, restore_requests: 3806,
    lookup_misses: 2, pendingMask: 1, keyNotReadyMask: 1, commitRequestedMask: 1 };
  const channel = new FeatureChannel(receiver.devices[0], options);
  await channel.open();
  const report = await readDiagnostics(channel);
  assert.equal(report.reconnect.firmwareRevision, 4);
  assert.equal(report.reconnect.bondCount, 0);
  assert.equal(report.reconnect.counters.restore_requests, 3806);
  assert.equal(report.reconnect.commitRequestedMask, 1);
  const hints = diagnosticHints(report);
  assert.equal(hints.some((hint) => hint.includes("绑定库为空")), true);
  assert.equal(hints.some((hint) => hint.includes("等待手柄以最终密钥")), true);
  assert.deepEqual(receiver.calls.map((item) => item.command), [0xf0, 0xf7, 0xf8]);
  assert.equal(JSON.stringify(report.reconnect).includes('"key"'), false);
  report.reconnect.bondCount = 20;
  report.reconnect.counters.commit_failures = 1;
  assert.equal(diagnosticHints(report).some((hint) => hint.includes("绑定库已达 20 条")), true);
});

test("revision6 records host readiness retries without exporting Feature payloads or keys", async () => {
  const receiver = new FakeReceiver({ version: 3, revision: 6 });
  receiver.hostStats = { setCommand: 0xae, getCommand: 0x83, replyLength: 50,
    query_busy: 73, query_ready: 2, invalid_sets: 1 };
  const channel = new FeatureChannel(receiver.devices[0], options);
  await channel.open();
  const report = await readDiagnostics(channel);
  assert.equal(report.hostLink.counters.query_busy, 73);
  assert.equal(report.hostLink.lastSet.command, 0xae);
  assert.equal(report.hostLink.lastGet.payloadLength, 50);
  assert.equal(report.hostLink.lastRejected, null); // v6 has no rejection metadata.
  assert.deepEqual(receiver.calls.map((call) => call.command), [0xf0, 0xf7, 0xf8, 0xfa]);
  assert.equal(JSON.stringify(report.hostLink).includes('"key"'), false);
  await channel.close();
});

test("revision9 exposes rejected header and reason while preserving FA v1 counters", async () => {
  const receiver = new FakeReceiver({ version: 3, revision: 9 });
  const channel = new FeatureChannel(receiver.devices[0], options);
  await channel.open();
  assert.equal((await readDiagnostics(channel)).hostLink.lastRejected, null);
  receiver.hostStats = { setCommand: 0x96, invalid_sets: 1, rejectedSlot: 2,
    rejectedCommand: 0x96, rejectedLength: 16, rejectedReason: 1 };
  const host = (await readDiagnostics(channel)).hostLink;
  assert.equal(host.version, 1);
  assert.equal(host.lastSet.command, 0x96);
  assert.equal(host.counters.invalid_sets, 1);
  assert.deepEqual(host.lastRejected, { slot: 2, command: 0x96, declaredLength: 16,
    reason: 1, reasonName: "报告帧或声明长度无效" });
  assert.equal(JSON.stringify(host).includes('"payload"'), false);
  receiver.hostStats.rejectedReason = 4;
  receiver.hostStats.rejectedCommand = receiver.hostStats.rejectedLength = 0;
  assert.equal((await readDiagnostics(channel)).hostLink.lastRejected.reasonName, "OUT 传输失败或缓冲无效");
  await channel.close();
});

test("revision10 reports real disconnect causes and final transport state separately from FA", async () => {
  const receiver = new FakeReceiver({ version: 3, revision: 10 });
  receiver.transportStats = { watchdog_disconnects: 23, host_disconnects: 2, ack_deferred: 70,
    stale_session_sets: 3, slots: [[1, 0, 2, 10, 96, 2]] };
  const channel = new FeatureChannel(receiver.devices[0], options);
  await channel.open();
  const report = await readDiagnostics(channel);
  assert.equal(report.transportLink.counters.watchdog_disconnects, 23);
  assert.equal(report.transportLink.counters.stale_session_sets, 3);
  assert.deepEqual(report.transportLink.slots[0], { slot: 0, connected: false,
    lastDisconnectReason: 1, reasonName: "无线看门狗超时", watchdog: 0, queued: 2,
    laneMask: 2, phaseMask: 8, rxCounterLow8: 96, txCounterLow8: 2 });
  assert.deepEqual(receiver.calls.map((call) => call.command), [0xf0, 0xf7, 0xf8, 0xfa, 0xfb]);
  assert.equal(JSON.stringify(report.transportLink).includes('"key"'), false);
  await channel.close();
});

test("revision11 routine diagnostics read only FC page zero, while old firmware sends no FC", async () => {
  for (const [version, revision] of [[3, 11], [3, 10], [3, 4], [2, 11]]) {
    const receiver = new FakeReceiver({ version, revision });
    receiver.queueStats = { clockUs: 0xfedcba98, usbHighWater: 31, clockAvailable: false,
      slots: [{ haptic_admitted: 0xf1234567, haptic_full: 3, usb_haptic_timeouts: 5, fifoHighWater: 12 }] };
    const channel = new FeatureChannel(receiver.devices[0], options);
    await channel.open();
    const diagnostics = await readDiagnostics(channel);
    const calls = receiver.calls.filter((call) => call.command === 0xfc);
    if (version === 3 && revision >= 11) {
      assert.equal(calls.length, 1);
      assert.deepEqual(calls[0].payload.slice(0, 3), [0xfc, 0, 0]);
      assert.equal(diagnostics.queueDiagnostics.detail, false);
      assert.equal(diagnostics.queueDiagnostics.slots, undefined);
      assert.equal(diagnostics.queueDiagnostics.events, undefined);
      assert.equal(diagnostics.queueDiagnostics.snapshotClockUs, 0xfedcba98);
      assert.equal(diagnostics.queueDiagnostics.clockAvailable, false);
      assert.equal(diagnostics.queueDiagnostics.hapticWaitBudgetUs, 8000);
      assert.equal(diagnostics.queueDiagnostics.counters.haptic_admitted_total, 0xf1234567);
      assert.equal(diagnostics.queueDiagnostics.counters.haptic_timeouts_total, 5);
      assert.equal(diagnostics.queueDiagnostics.rfHighWater, 12);
      assert.equal(diagnostics.queueDiagnostics.usbHighWater, 31);
    } else {
      assert.deepEqual(calls, []);
      assert.equal(diagnostics.queueDiagnostics, undefined);
    }
    await channel.close();
  }
});

test("full FC pages retain one ID/time and frozen counters despite live backend changes", async () => {
  const receiver = new FakeReceiver({ version: 3, revision: 11 });
  receiver.queueId = 0x12345677;
  const slot = { admitted: 101, haptic_admitted: 71, haptic_full: 2, a2_full: 3, a3_full: 4, a4_full: 5,
    a5_full: 6, a6_full: 7, other_full: 8, initial_retries: 9, count_retries: 10, commit_waits: 11,
    offline: 12, serialized: 13, retired: 14, cleared_fragments: 15, max_fifo_age_us: 1600, max_lane_age_us: 1700,
    usb_admitted: 18, usb_processed: 19, usb_haptic_received: 20, usb_haptic_waits: 21, usb_haptic_timeouts: 22,
    usb_haptic_stale: 23, fifoHighWater: 12, pendingHaptics: 2, usb_max_age_us: 2400,
    usb_haptic_offline: 25, usb_haptic_reset_cancelled: 26 };
  const events = [0, 1, 2].map((i) => ({ id: (0xffffffff + i) >>> 0, at_us: 100 + i, generation: 0xabcdef01,
    age_us: 8000 + i, slot: i, kind: 7, command: 0x8f, length: 11, needed: 2, fifo_count: 12, lane_flags: 15, usb_depth: 31 }));
  receiver.queueStats = { clockUs: 0xfffffff0, slots: Array.from({ length: 4 }, () => ({ ...slot })), events,
    usbDepth: 30, usbHighWater: 31, maxRxLatencyUs: 2800, maxProcessGapUs: 2900 };
  const original = receiver.report.bind(receiver), headers = [];
  receiver.report = (...args) => {
    const reply = original(...args);
    if (args[1] === 0xfc) {
      headers.push([...reply.slice(6, 14)]);
      receiver.queueStats.slots.forEach((s) => { s.haptic_admitted += 100; s.usb_max_age_us += 100; });
      receiver.queueStats.clockUs++;
      receiver.queueStats.events = [];
    }
    return reply;
  };
  const channel = new FeatureChannel(receiver.devices[3], options);
  await channel.open();
  const snapshot = await readQueueDiagnostics(channel, { detail: true });
  assert.equal(snapshot.snapshotId, 0x12345678);
  assert.equal(snapshot.snapshotClockUs, 0xfffffff0);
  assert.equal(headers.every((header) => JSON.stringify(header) === JSON.stringify(headers[0])), true);
  assert.deepEqual(snapshot.slots, Array.from({ length: 4 }, (_, i) => ({ slot: i, ...slot })));
  assert.deepEqual(snapshot.events, events.map((event) => ({ ...event, kindName: "UsbTimeout" })));
  assert.equal(snapshot.counters.haptic_admitted_total, 284);
  assert.equal(snapshot.counters.a2_a6_full_total, 100);
  assert.equal(snapshot.counters.control_retries_total, 76);
  assert.equal(snapshot.counters.max_usb_age_us, 2400);
  assert.equal(snapshot.counters.max_rx_latency_us, 2800);
  assert.equal(snapshot.counters.max_process_gap_us, 2900);
  assert.equal(snapshot.usbDepth, 30);
  assert.equal(snapshot.usbBatch, 4);
  assert.deepEqual(receiver.calls.map((call) => call.payload.slice(0, call.payload[1] + 2)),
    [[0xfc, 0], ...[1, 2, 3, 4, 5, 6, 7, 8, 17, 9, 10].map((page) => [0xfc, 5, page, 0x78, 0x56, 0x34, 0x12])]);
  await channel.close();
});

test("FC detail reads only populated event pages, including an empty and a full ring", async () => {
  for (const count of [0, 16]) {
    const receiver = new FakeReceiver({ version: 3, revision: 11 });
    receiver.queueStats.events = Array.from({ length: count }, (_, i) => ({ id: i + 1, slot: i % 4, kind: i % 9 + 1 }));
    const channel = new FeatureChannel(receiver.devices[0], options);
    await channel.open();
    const snapshot = await readQueueDiagnostics(channel, { detail: true });
    assert.equal(snapshot.events.length, count);
    assert.deepEqual(receiver.calls.map((call) => call.payload[2]),
      [0, 1, 2, 3, 4, 5, 6, 7, 8, 17, ...Array.from({ length: count / 2 }, (_, i) => 9 + i)]);
    await channel.close();
  }
});

test("expired FC token discards all earlier pages and restarts the full read exactly once", async () => {
  for (const expireAlways of [false, true]) {
    const receiver = new FakeReceiver({ version: 3, revision: 11 });
    receiver.queueStats.slots = Array.from({ length: 4 }, () => ({ haptic_admitted: 1, usb_processed: 1 }));
    const original = receiver.report.bind(receiver);
    let expirations = 0;
    receiver.report = (slot, command, args) => {
      if (command === 0xfc && args[0] === 17 && (!expirations || expireAlways)) {
        ++expirations;
        receiver.queueStats.slots.forEach((s) => { s.haptic_admitted = s.usb_processed = 2; });
        original(slot, command, []); // A competing tool freezes a newer snapshot.
      }
      return original(slot, command, args);
    };
    const channel = new FeatureChannel(receiver.devices[0], options);
    await channel.open();
    if (expireAlways) await assert.rejects(readQueueDiagnostics(channel, { detail: true }), QueueSnapshotExpiredError);
    else {
      const snapshot = await readQueueDiagnostics(channel, { detail: true });
      assert.equal(snapshot.snapshotId, 3);
      assert.equal(snapshot.snapshotClockUs, 3000);
      assert.equal(snapshot.counters.haptic_admitted_total, 8);
      assert.equal(snapshot.slots.every((s) => s.haptic_admitted === 2 && s.usb_processed === 2), true);
    }
    assert.equal(receiver.calls.filter((call) => call.payload[1] === 0).length, 2);
    assert.equal(receiver.calls.length, 20);
    assert.equal(channel.closed, false);
    await channel.close();
  }
});

test("malformed FC headers, mixed snapshot ID/time and bad event counts never return partial data", async (t) => {
  const cases = [
    ["size", 1, 61], ["schema", 2, 2], ["page", 3, 2], ["revision", 4, 10],
    ["bad selector", 5, 1], ["unknown status", 5, 3], ["ID", 6, 99], ["clock", 10, 99],
  ];
  for (const [name, offset, value] of cases) await t.test(name, async () => {
    const receiver = new FakeReceiver({ version: 3, revision: 11 });
    const original = receiver.report.bind(receiver);
    receiver.report = (slot, command, args) => {
      const reply = original(slot, command, args);
      if (command === 0xfc && args[0] === 1) reply[offset] = value;
      return reply;
    };
    const channel = new FeatureChannel(receiver.devices[0], options);
    await channel.open();
    await assert.rejects(readQueueDiagnostics(channel, { detail: true }), /FC/);
    assert.equal(receiver.calls.length, 2); // Only expiry status is retryable.
    await channel.close();
  });
  const receiver = new FakeReceiver({ version: 3, revision: 11 });
  receiver.queueStats.events = [{ kind: 1 }];
  const original = receiver.report.bind(receiver);
  receiver.report = (slot, command, args) => {
    const reply = original(slot, command, args);
    if (args[0] === 9) reply[62] = 2;
    return reply;
  };
  const channel = new FeatureChannel(receiver.devices[0], options);
  await channel.open();
  await assert.rejects(readQueueDiagnostics(channel, { detail: true }), /行数/);
  await channel.close();
});

test("binding snapshots and transactional import include receiver identity and readback", async () => {
  const receiver = new FakeReceiver({ bonds: [bond] });
  const channel = new FeatureChannel(receiver.devices[0], options);
  await channel.open();
  const before = await exportBackup(channel);
  assert.equal(before.receiverAddress, "2836576294");
  assert.deepEqual(before.bonds, [bond]);
  assert.equal(receiver.transfer, null);
  const progress = [];
  const after = await importBackup(channel, backup, (current, total) => progress.push([current, total]));
  assert.deepEqual(after, backup);
  assert.deepEqual(progress, [[1, 1]]);
  assert.equal(receiver.address, backup.receiverAddress);
  assert.equal(receiver.transfer, null);
  assert.equal(receiver.calls.filter((item) => item.command === 0xf5).length, 1);
});

test("a failed Flash commit cancels staging and keeps old bindings", async () => {
  const receiver = new FakeReceiver({ bonds: [bond] });
  receiver.failCommit = true;
  const channel = new FeatureChannel(receiver.devices[0], options);
  await channel.open();
  await assert.rejects(importBackup(channel, backup), /Flash/);
  assert.equal(receiver.address, "2836576294");
  assert.deepEqual(receiver.bonds, [bond]);
  assert.equal(receiver.transfer, null);
});

test("invalid/duplicate backup data is rejected before any USB staging", async () => {
  const receiver = new FakeReceiver();
  const channel = new FeatureChannel(receiver.devices[0], options);
  await channel.open();
  await assert.rejects(importBackup(channel, { ...backup, bonds: [bond, bond] }), /重复/);
  await assert.rejects(importBackup(channel, { ...backup, receiverAddress: "ab12345678" }), /低三位/);
  assert.equal(receiver.calls.length, 0);
  assert.throws(() => validateBackup({ ...backup, bonds: [{ ...bond, key: "00" }] }), /32/);
  assert.throws(() => validateBackup({ format: "openpuck", version: 1, bonds: [] }), /SC2015/);
});

function journalRecord(pages, page, index, id, key) {
  const offset = page * 512 + index * 25;
  pages[offset] = 0xf0;
  pages.set(bytesOf(id), offset + 1);
  pages.set(bytesOf(key), offset + 9);
}

test("journal parser uses the full first record boot rule, scans holes, and deduplicates newest within one page", () => {
  const pages = new Uint8Array(1024).fill(255);
  journalRecord(pages, 0, 0, bond.controllerId, "11".repeat(16));
  journalRecord(pages, 0, 19, bond.controllerId, bond.key);
  journalRecord(pages, 1, 19, "22".repeat(8), "33".repeat(16));
  const result = parseJournalDump(pages, backup.receiverAddress);
  assert.equal(result.activePage, 0x7400);
  assert.deepEqual(result.backup.bonds, [bond]);
  assert.equal(result.superseded, 1);
  assert.equal(result.pages[1].valid, 1);
  pages[512] = 0;
  const changed = parseJournalDump(pages, backup.receiverAddress);
  assert.equal(changed.activePage, 0x7600);
  assert.equal(changed.backup.bonds[0].controllerId, "22".repeat(8));
  const full = new Uint8Array(0x8000).fill(255);
  full.set(pages, 0x7400);
  assert.deepEqual(parseJournalDump(full, backup.receiverAddress), changed);
  assert.throws(() => parseJournalDump(new Uint8Array(19118), backup.receiverAddress), /不含绑定页/);
});
