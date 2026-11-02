import test from "node:test";
import assert from "node:assert/strict";
import { FeatureChannel, COMMAND, commandBytes, featureBytes, parseDiagnostics, parseIdentity,
  readDiagnostics, counterDelta, parseInput, exportBackup, importBackup, validateBackup, parseJournalDump,
  diagnosticHints } from "../web/protocol.mjs";
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
