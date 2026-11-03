import test from "node:test";
import assert from "node:assert/strict";
import { FeatureChannel, isReceiver, readDiagnostics, readInputMapping, saveInputMapping, switchInputMode,
  defaultInputMapping, encodeInputMapping, inputMappingToken, parseInputMapping, parseInputMode } from "../web/protocol.mjs";
import { ReceiverManager } from "../web/manager.mjs";
import { FakeReceiver, FakeHid } from "./web/mock_hid.mjs";
const options = { wait: async () => {}, attempts: 3 };
async function channel(backend) {
  const c = new FeatureChannel(backend.devices[0], options); await c.open(); return c;
}
test("revision12 mode and mapping default wire layout match the requested controls", async () => {
  const backend = new FakeReceiver({ version: 3, revision: 12 });
  const c = await channel(backend), diag = await readDiagnostics(c), mapping = await readInputMapping(c);
  assert.equal(diag.inputMode.mode, 0); assert.equal(diag.inputMode.resetOnUnplug, true);
  assert.equal(mapping.bindings[9], 11); assert.equal(mapping.bindings[10], 10); assert.equal(mapping.bindings[13], 11);
  assert.equal(mapping.requireLeftPadClick, true);
  assert.deepEqual(encodeInputMapping(mapping), backend.mapping);
  assert.equal(inputMappingToken(new TextEncoder().encode("123456789")), 0xcbf43926);
  await c.close();
});
test("revision14 endpoint diagnostics retain unknown host recognition and validate the extension", () => {
  const b = new FakeReceiver({ version: 3, revision: 12 }).report(0, 0xfd, []);
  assert.equal(parseInputMode(b).xinput, null);
  b[3] = 14; b[5] = b[6] = 1; b[9] = 1; b[10] = 7;
  const view = new DataView(b.buffer, b.byteOffset, b.byteLength);
  view.setUint32(28, 123, true); view.setUint32(32, 122, true); view.setUint32(36, 1, true);
  b.set([0xa1,1,0,1,0,0,20,0], 52);
  const x = parseInputMode(b).xinput;
  assert.equal(x.opened, true); assert.equal(x.inSubmitted, 123); assert.equal(x.inCompleted, 122);
  assert.equal(x.inFailed, 1); assert.equal(x.lastSetup, "a101000100001400");
  for (const [offset, value] of [[9,0], [9,2], [10,16], [11,1]]) {
    const bad = b.slice(); bad[offset] = value; assert.throws(() => parseInputMode(bad), /诊断字段/);
  }
});
test("mapping save is checked, stale writes and failed persistence preserve the active mapping", async () => {
  const backend = new FakeReceiver({ version: 3, revision: 12 });
  const c = await channel(backend), before = await readInputMapping(c);
  const draft = defaultInputMapping(12); draft.bindings[9] = 1; draft.padDeadzone = 5000; draft.invertRightY = true; draft.requireLeftPadClick = false;
  const saved = await saveInputMapping(c, before, draft);
  assert.equal(saved.bindings[9], 1); assert.equal(saved.padDeadzone, 5000); assert.equal(saved.invertRightY, true);
  assert.equal(saved.requireLeftPadClick, false);
  assert.equal(backend.configSaves, 1);
  await assert.rejects(saveInputMapping(c, before, draft), /重新读取/);
  backend.failConfigSave = true;
  await assert.rejects(saveInputMapping(c, saved, defaultInputMapping(12)), /保存失败/);
  assert.deepEqual(await readInputMapping(c), saved);
  const count = backend.calls.length;
  draft.bindings[0] = 250;
  assert.throws(() => encodeInputMapping(draft), /无效/);
  assert.equal(backend.calls.length, count);
  await c.close();
});
test("mapping parser rejects corrupt payloads and wrong schema before using fields", async () => {
  const b = new FakeReceiver({ version: 3, revision: 12 });
  for (const [offset, value] of [[2,2], [5,23], [12,2], [13,8], [14,255], [8,0]]) {
    const reply = b.report(0, 0xfe, []); reply[offset] = value;
    assert.throws(() => parseInputMapping(reply));
  }
});
test("mode command confirms the target and the XInput configuration HID remains discoverable", async () => {
  const b = new FakeReceiver({ version: 3, revision: 12 });
  const c = await channel(b);
  assert.equal((await switchInputMode(c, 1)).requestedMode, 1);
  assert.equal(b.inputMode, 1); assert.equal(isReceiver(b.devices[0]), true);
  assert.equal(b.devices[0].productId, 0x1143);
  await assert.rejects(switchInputMode(c, 2), /不支持/);
  await c.close();
});
test("manager mode switch releases old handles, reauthorizes the new identity and retains saved mappings", async () => {
  const b = new FakeReceiver({ version: 3, revision: 12 }), hid = new FakeHid(b.devices);
  const manager = new ReceiverManager(hid, options);
  await manager.request();
  assert.equal(hid.filters.length, 2);
  const draft = defaultInputMapping(12); draft.bindings[10] = 4;
  await manager.enqueue(() => manager.saveInputMapping(b.address, draft));
  await manager.enqueue(() => manager.switchInputMode(b.address, 1));
  assert.equal(manager.entries.size, 0); assert.equal(b.devices.every((d) => !d.opened), true);
  await manager.request();
  assert.equal(manager.groups()[0].diagnostic.inputMode.mode, 1);
  assert.equal(manager.groups()[0].inputMapping.bindings[10], 4);
  await manager.enqueue(() => manager.switchInputMode(b.address, 0));
  await manager.request(); assert.equal(manager.groups()[0].diagnostic.inputMode.mode, 0);
  await manager.destroy();
});
test("old firmware and original donors never receive mode or mapping extensions", async () => {
  for (const props of [{ version:3, revision:11 }, { original:true }]) {
    const b = new FakeReceiver(props), manager = new ReceiverManager(new FakeHid(b.devices), options);
    await manager.request();
    await assert.rejects(manager.switchInputMode(b.address, 1));
    await assert.rejects(manager.loadInputMapping(b.address));
    assert.equal(b.calls.some((c) => c.command >= 0xfd), false);
    await manager.destroy();
  }
});
test("revision16 persists a one-shot, reboots without Xbox HID, and returns to Steam on next power cycle", async () => {
  const b = new FakeReceiver({ version: 3, revision: 16 }), hid = new FakeHid(b.devices);
  const manager = new ReceiverManager(hid, options);
  await manager.request();
  assert.equal(manager.groups()[0].inputMapping.persistMode, false);
  await manager.enqueue(() => manager.switchInputMode(b.address, 1));
  assert.equal(b.reboots, 1); assert.equal(b.modeSaves, 1); assert.equal(b.inputMode, 1);
  assert.equal(manager.entries.size, 0); assert.deepEqual(await hid.getDevices(), []);
  assert.equal(b.devices[0].vendorId, 0x045e); assert.equal(b.devices[0].productId, 0x028e);
  b.powerCycle(); await manager.request();
  assert.equal(manager.groups()[0].diagnostic.inputMode.mode, 0);
  const draft = defaultInputMapping(); draft.persistMode = true;
  await manager.enqueue(() => manager.saveInputMapping(b.address, draft));
  await manager.enqueue(() => manager.switchInputMode(b.address, 1));
  b.powerCycle(); assert.equal(b.inputMode, 1); assert.deepEqual(await hid.getDevices(), []);
  b.restartMode(0); await manager.request();
  assert.equal(manager.groups()[0].diagnostic.inputMode.persistMode, true);
  assert.equal(manager.groups()[0].diagnostic.inputMode.resetOnUnplug, false);
  await manager.destroy();
});
test("mode persistence is version gated and a failed boot save remains visible in Steam", async () => {
  const old = new FakeReceiver({ version: 3, revision: 15 }), oldChannel = await channel(old);
  const previous = await readInputMapping(oldChannel), draft = defaultInputMapping(); draft.persistMode = true;
  const calls = old.calls.length;
  await assert.rejects(saveInputMapping(oldChannel, previous, draft), /修订号 16/);
  assert.equal(old.calls.length, calls); await oldChannel.close();
  const b = new FakeReceiver({ version: 3, revision: 16 }), manager = new ReceiverManager(new FakeHid(b.devices), options);
  await manager.request(); b.failModeSave = true;
  await manager.enqueue(() => manager.switchInputMode(b.address, 1));
  assert.equal(b.reboots, 0); assert.equal(b.inputMode, 0);
  await manager.request(); assert.equal(manager.groups()[0].diagnostic.inputMode.switchSaveFailed, true);
  await manager.destroy();
});
test("diagonal width saves independently of deadzone and preserves every angle encoding", async () => {
  const b = new FakeReceiver({ version: 3, revision: 16 }), c = await channel(b);
  let mapping = await readInputMapping(c);
  assert.equal(mapping.diagonalAngle, 30);
  const draft = defaultInputMapping(); draft.diagonalAngle = 15; draft.persistMode = true;
  mapping = await saveInputMapping(c, mapping, draft);
  assert.equal(mapping.diagonalAngle, 15); assert.equal(mapping.dpadThreshold, 8000);
  assert.equal(mapping.persistMode, true); assert.equal((await readInputMapping(c)).diagonalAngle, 15);
  for (let angle = 0; angle <= 75; angle += 5) {
    draft.diagonalAngle = angle;
    mapping = await saveInputMapping(c, mapping, draft);
    assert.equal(mapping.diagonalAngle, angle); assert.equal(mapping.persistMode, true);
    assert.equal(mapping.requireLeftPadClick, true);
  }
  for (const angle of [-5, 31, 80, "30"]) {
    draft.diagonalAngle = angle; assert.throws(() => encodeInputMapping(draft), /斜向角度/);
  }
  await c.close();
  const old = new FakeReceiver({ version: 3, revision: 15 }), oldChannel = await channel(old);
  const previous = await readInputMapping(oldChannel);
  assert.equal(previous.diagonalAngle, 45);
  await assert.rejects(saveInputMapping(oldChannel, previous, defaultInputMapping()), /斜向区域调整需要/);
  await oldChannel.close();
});
test("revision17 switches into PS5, preserves the common mapping and remembers PS5 only when requested", async () => {
  const b = new FakeReceiver({ version: 3, revision: 17 }), hid = new FakeHid(b.devices);
  const manager = new ReceiverManager(hid, options);
  await manager.request();
  const draft = defaultInputMapping(); draft.diagonalAngle = 20;
  await manager.enqueue(() => manager.saveInputMapping(b.address, draft));
  const result = await manager.enqueue(() => manager.switchInputMode(b.address, 2));
  assert.equal(result.requestedMode, 2); assert.equal(b.inputMode, 2);
  assert.equal(b.devices[0].vendorId, 0x054c); assert.equal(b.devices[0].productId, 0x0ce6);
  assert.equal(manager.entries.size, 0); assert.deepEqual(await hid.getDevices(), []);
  b.powerCycle(); await manager.request(); assert.equal(b.inputMode, 0);
  assert.equal(manager.groups()[0].inputMapping.diagonalAngle, 20);
  draft.persistMode = true; await manager.enqueue(() => manager.saveInputMapping(b.address, draft));
  await manager.enqueue(() => manager.switchInputMode(b.address, 2)); b.powerCycle(); assert.equal(b.inputMode, 2);
  b.restartMode(0); await manager.request(); assert.equal(b.inputMode, 0);
  await manager.destroy();
});
test("older firmware never receives a PS5 mode write", async () => {
  for (const revision of [12,16]) {
    const b = new FakeReceiver({ version: 3, revision }), c = await channel(b);
    await assert.rejects(switchInputMode(c, 2), /不支持 PS5/);
    assert.equal(b.calls.some((call) => call.command === 0xfd && call.payload[1] !== 0), false);
    await c.close();
  }
});
