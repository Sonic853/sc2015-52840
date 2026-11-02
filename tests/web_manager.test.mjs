import test from "node:test";
import assert from "node:assert/strict";
import { ReceiverManager } from "../web/manager.mjs";
import { FakeReceiver, FakeHid } from "./web/mock_hid.mjs";

const options = { wait: async () => {}, attempts: 5 };

test("four slot identities are read from F0, not the device picker order", async () => {
  const backend = new FakeReceiver();
  const hid = new FakeHid([...backend.devices].reverse());
  const manager = new ReceiverManager(hid, options);
  await manager.request();
  assert.equal(manager.entries.size, 4);
  assert.deepEqual(manager.groups()[0].entries.map((entry) => entry.info.slot), [0, 1, 2, 3]);
  assert.equal(hid.filters[0].usagePage, 0xff00);
  await manager.destroy();
  assert.equal(manager.entries.size, 0);
  assert.equal(backend.devices.every((device) => !device.opened), true);
});

test("original donor is identity-only, receives no SC2 or XIAO-specific command", async () => {
  const donor = new FakeReceiver({ original: true, address: "ab12345678" });
  const hid = new FakeHid(donor.devices);
  const manager = new ReceiverManager(hid, options);
  await manager.request();
  await manager.refresh();
  assert.equal(manager.groups()[0].kind, "original");
  assert.equal(manager.groups()[0].id, "a812345678");
  assert.equal(manager.entries.size, 1);
  assert.equal(donor.calls.every((item) => item.command === 0xa1), true);
  await manager.destroy();
});

test("auto-save acts only on new pending slots in the receiver whose pairing this page started", async () => {
  const first = new FakeReceiver();
  const second = new FakeReceiver({ address: "8033445566" });
  first.states[1] = 3;
  const manager = new ReceiverManager(new FakeHid([...first.devices, ...second.devices]), options);
  await manager.request();
  first.states[2] = 3;
  await manager.pairing(first.address, true, true);
  first.input(0, 3);
  second.input(0, 3);
  await manager.refresh();
  assert.equal(first.states[0], 2);
  assert.equal(first.states[1], 3);
  assert.equal(first.states[2], 3);
  assert.equal(second.states[0], 3);
  assert.deepEqual(first.calls.filter((item) => item.command === 0xb3).map((item) => item.slot), [0]);
  await manager.refresh();
  assert.equal(first.calls.filter((item) => item.command === 0xb3).length, 1);
  await manager.destroy();
});

test("failed auto-save does not retry Flash writes on every diagnostic poll", async () => {
  const backend = new FakeReceiver();
  backend.failCommit = true;
  const manager = new ReceiverManager(new FakeHid(backend.devices), options);
  await manager.request();
  await manager.pairing(backend.address, true, true);
  backend.input(0, 3);
  await manager.refresh();
  await manager.refresh();
  assert.equal(backend.states[0], 3);
  assert.equal(backend.calls.filter((item) => item.command === 0xb3).length, 1);
  await manager.destroy();
});

test("save waits for a deferred final-key commit and sends B3 only once", async () => {
  const backend = new FakeReceiver({ version: 3, revision: 4 });
  backend.states[0] = 3;
  backend.deferCommit = true;
  const manager = new ReceiverManager(new FakeHid([backend.devices[0]]), options);
  await manager.request();
  const entry = [...manager.entries.values()][0];
  await manager.commit(entry);
  assert.equal(entry.state, 2);
  assert.equal(backend.calls.filter((item) => item.command === 0xb3).length, 1);
  assert.equal(backend.calls.some((item) => item.command === 0xf8), true);
  await manager.destroy();
});

test("unplug removes the interface and cancels its automatic pairing scope", async () => {
  const backend = new FakeReceiver();
  const hid = new FakeHid([backend.devices[0]]);
  const manager = new ReceiverManager(hid, options);
  await manager.request();
  await manager.pairing(backend.address, true, true);
  hid.emit("disconnect", backend.devices[0]);
  assert.equal(manager.entries.size, 0);
  assert.equal(manager.armed.size, 0);
  await manager.destroy();
});

test("a donor and imported XIAO with equal RF identities stay separate groups", async () => {
  const backend = new FakeReceiver();
  const donor = new FakeReceiver({ address: backend.address, original: true });
  const manager = new ReceiverManager(new FakeHid([backend.devices[0], donor.devices[0]]), options);
  await manager.request();
  assert.deepEqual(manager.groups().map((item) => item.kind).sort(), ["original", "xiao"]);
  await manager.destroy();
});

test("import re-reads and applies the new identity to all target interfaces", async () => {
  const backend = new FakeReceiver();
  const manager = new ReceiverManager(new FakeHid(backend.devices), options);
  await manager.request();
  const data = { format: "sc2015-receiver-backup", version: 1, receiverAddress: "a812345678",
    bonds: [{ controllerId: "01".repeat(8), key: "a7".repeat(16) }] };
  await manager.import(backend.address, data);
  assert.equal(manager.groups().length, 1);
  assert.equal(manager.groups()[0].id, data.receiverAddress);
  assert.equal(manager.groups()[0].entries.length, 4);
  await manager.refresh();
  await manager.destroy();
});

test("a missing Feature reply retains the live slot and recovers on the next refresh", async () => {
  const backend = new FakeReceiver({ version: 3, revision: 5 });
  backend.states.fill(2);
  const manager = new ReceiverManager(new FakeHid(backend.devices), options);
  await manager.request();
  const entry = manager.entries.get(backend.devices[0]);
  backend.devices[0].overrides.push(...Array.from({ length: 5 }, () => new Uint8Array(64)));
  await manager.refresh();
  assert.equal(manager.entries.size, 4);
  assert.equal(entry.state, 2);
  assert.match(entry.queryError, /Feature/);
  assert.equal(entry.channel.closed, false);
  const bytes = new Uint8Array(64);
  bytes.set([1, 0, 1, 60]);
  backend.devices[0].emitInput(bytes);
  assert.equal(entry.inputCount, 1);
  await manager.refresh();
  assert.equal(entry.queryError, null);
  assert.equal(entry.state, 2);
  await manager.destroy();
});

test("modern polling uses an idle interface and F0/F8 masks without emitting B4 events", async () => {
  const backend = new FakeReceiver({ version: 3, revision: 6 });
  backend.states[0] = 2;
  const manager = new ReceiverManager(new FakeHid(backend.devices), options);
  await manager.request();
  backend.calls.length = 0;
  backend.states[2] = 3;
  backend.reconnectStats.pendingMask = 4;
  await manager.refresh();
  assert.deepEqual(backend.calls.map((call) => [call.slot, call.command]), [[1, 0xf0], [1, 0xf7], [1, 0xf8], [1, 0xfa]]);
  assert.deepEqual(manager.groups()[0].entries.map((entry) => entry.state), [2, 1, 3, 1]);
  await manager.destroy();
});

test("hung USB is quarantined without erasing state and explicit authorization reopens it", async () => {
  const backend = new FakeReceiver();
  backend.states[0] = 2;
  const manager = new ReceiverManager(new FakeHid(backend.devices), { ...options, timeout: 20 });
  await manager.request();
  const device = backend.devices[0];
  const receive = device.receiveFeatureReport.bind(device);
  let reads = 0;
  device.receiveFeatureReport = () => { ++reads; return new Promise(() => {}); };
  await manager.refresh();
  const entry = manager.entries.get(device);
  assert.equal(entry.state, 2);
  assert.equal(entry.channel.closed, true);
  assert.match(entry.queryError, /暂停/);
  await manager.refresh();
  assert.equal(reads, 1);
  device.receiveFeatureReport = receive;
  await manager.request();
  assert.equal(manager.entries.size, 4);
  assert.equal(manager.entries.get(device).channel.closed, false);
  assert.equal(manager.entries.get(device).queryError, null);
  await manager.destroy();
});

test("UF2 mode targets only revision5 XIAO and releases all of that receiver's handles", async () => {
  const backend = new FakeReceiver({ version: 3, revision: 5 });
  const other = new FakeReceiver({ version: 3, revision: 5, address: "8033445566" });
  const donor = new FakeReceiver({ original: true, address: backend.address });
  const manager = new ReceiverManager(new FakeHid([...backend.devices, ...other.devices, ...donor.devices]), options);
  await manager.request();
  await manager.pairing(backend.address, true, true);
  manager.entries.get(backend.devices[0]).queryError = "previous incomplete reply";
  await manager.bootloader(backend.address);
  const resets = backend.calls.filter((item) => item.command === 0xf9);
  assert.equal(resets.length, 1);
  assert.equal(resets[0].slot, 1);
  assert.deepEqual(resets[0].payload.slice(0, 6), [0xf9, 4, 0x55, 0x46, 0x32, 0x21]);
  assert.equal(backend.bootloader, true);
  assert.equal(backend.devices.every((device) => !device.opened), true);
  assert.equal(manager.armed.has(backend.address), false);
  assert.equal(manager.groups().filter((group) => group.kind === "xiao").length, 1);
  assert.equal(other.calls.some((item) => item.command === 0xf9), false);
  assert.equal(donor.calls.every((item) => item.command === 0xa1), true);
  await manager.destroy();
});

test("old firmware and an active import cannot enter UF2 mode", async () => {
  const backend = new FakeReceiver({ version: 3, revision: 4 });
  const manager = new ReceiverManager(new FakeHid(backend.devices), options);
  await manager.request();
  await assert.rejects(manager.bootloader(backend.address), /修订号 5/);
  assert.equal(backend.calls.some((item) => item.command === 0xf9), false);
  backend.revision = 5;
  backend.transfer = { type: "import" };
  await assert.rejects(manager.bootloader(backend.address), /导入尚未结束/);
  assert.equal(manager.entries.size, 4);
  assert.equal(backend.bootloader, false);
  assert.equal(backend.devices.every((device) => device.opened), true);
  await manager.destroy();
});
