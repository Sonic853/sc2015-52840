import test from "node:test";
import assert from "node:assert/strict";
import { FeatureChannel, isReceiver, readDiagnostics, readInputMapping, saveInputMapping, switchInputMode,
  defaultInputMapping, encodeInputMapping, inputMappingToken, parseInputMapping } from "../web/protocol.mjs";
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
test("mapping save is checked, stale writes and failed persistence preserve the active mapping", async () => {
  const backend = new FakeReceiver({ version: 3, revision: 12 });
  const c = await channel(backend), before = await readInputMapping(c);
  const draft = defaultInputMapping(); draft.bindings[9] = 1; draft.padDeadzone = 5000; draft.invertRightY = true; draft.requireLeftPadClick = false;
  const saved = await saveInputMapping(c, before, draft);
  assert.equal(saved.bindings[9], 1); assert.equal(saved.padDeadzone, 5000); assert.equal(saved.invertRightY, true);
  assert.equal(saved.requireLeftPadClick, false);
  assert.equal(backend.configSaves, 1);
  await assert.rejects(saveInputMapping(c, before, draft), /重新读取/);
  backend.failConfigSave = true;
  await assert.rejects(saveInputMapping(c, saved, defaultInputMapping()), /保存失败/);
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
  const draft = defaultInputMapping(); draft.bindings[10] = 4;
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
