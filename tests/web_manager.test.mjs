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
