import { test, expect } from "@playwright/test";
import { readFile } from "node:fs/promises";

const peer = (await readFile(new URL("../web/mock_hid.mjs", import.meta.url), "utf8")).replace(/^export /gm, "");
const bond = { controllerId: "0102030405060708", key: "00112233445566778899aabbccddeeff" };

async function install(page, { version = 2, revision = 3, donor = false, unsupported = false } = {}) {
  await page.addInitScript({ content: `(() => {
    ${peer}
    const backend = new FakeReceiver({ version: ${version}, revision: ${revision}, bonds: [${JSON.stringify(bond)}] });
    if (${version} === 3) {
      backend.counters[0] = backend.counters[7] = 2076;
      backend.pairingStats = { pipeRx: [2076, 0, 0, 0, 0, 0], length: 9, s1: 3,
        scheduled: 2076, ready: 2076, completed: 2076, ackLength: 22, irqDelay: 4, readyDelay: 132 };
    }
    const donor = new FakeReceiver({ original: true, address: 'ab12345678' });
    const devices = ${donor ? "[...backend.devices, ...donor.devices]" : "[...backend.devices].reverse()"};
    const hub = new FakeHid(devices);
    const request = hub.requestDevice.bind(hub);
    hub.requestDevice = (...args) => { hub.gesture = navigator.userActivation.isActive; return request(...args); };
    Object.defineProperty(navigator, 'hid', { configurable: true, value: ${unsupported ? "undefined" : "hub"} });
    window.__testHid = { backend, donor, hub };
  })();` });
  const errors = [];
  page.on("pageerror", (error) => errors.push(error.message));
  await page.goto("/");
  return errors;
}

test("desktop connects four correctly identified slots, pairs and auto-saves once", async ({ page }) => {
  const errors = await install(page);
  await page.getByRole("button", { name: "连接 / 授权接口" }).click();
  await expect(page.locator("#connection-status")).toContainText("1 只 XIAO");
  expect(await page.evaluate(() => window.__testHid.hub.gesture)).toBe(true);
  await expect(page.locator("#receiver-select")).toHaveValue("2836576294");
  await expect(page.locator("#slots .slot")).toHaveCount(4);
  await page.getByRole("button", { name: "开启配对", exact: true }).click();
  await expect(page.locator("#pairing-indicator")).toHaveText("配对已开启");
  await page.evaluate(() => window.__testHid.backend.input(0, 3));
  await expect(page.locator("#slots .slot").nth(0).locator(".pill")).toHaveText("已连接 · 绑定已保存");
  expect(await page.evaluate(() => window.__testHid.backend.calls.filter((call) => call.command === 0xb3).length)).toBe(1);
  await page.screenshot({ path: "test-results/console-desktop.png", fullPage: true });
  await page.getByRole("button", { name: "断开网页", exact: true }).click();
  await expect(page.locator("#connection-status")).toHaveText("尚未连接接收器");
  expect(errors).toEqual([]);
});

test("input packets, unplug and diagnostics download update without exposing binding keys", async ({ page }) => {
  const errors = await install(page);
  await page.getByRole("button", { name: "连接 / 授权接口" }).click();
  await expect(page.locator("#connection-status")).toContainText("1 只 XIAO");
  await page.evaluate(() => {
    const bytes = new Uint8Array(64);
    bytes.set([1, 0, 1, 60]);
    new DataView(bytes.buffer).setUint32(4, 17, true);
    window.__testHid.backend.devices[0].emitInput(bytes);
  });
  await expect(page.locator("#slots .slot").first().locator(".slot-detail")).toContainText("1 个输入报告");
  const downloaded = page.waitForEvent("download");
  await page.getByRole("button", { name: "下载诊断 JSON" }).click();
  const diagnostics = JSON.parse(await readFile(await (await downloaded).path(), "utf8"));
  expect(diagnostics.groups[0].slots[0].lastInput.sequence).toBe(17);
  expect(JSON.stringify(diagnostics)).not.toContain(bond.key);
  await page.evaluate(() => window.__testHid.hub.emit("disconnect", window.__testHid.backend.devices[0]));
  await expect(page.locator("#slots .slot").first().locator(".pill")).toHaveText("未授权接口");
  expect(errors).toEqual([]);
});

test("local backup downloads the complete identity and bindings", async ({ page }) => {
  await install(page);
  await page.getByRole("button", { name: "连接 / 授权接口" }).click();
  await expect(page.locator("#backup")).toBeEnabled();
  const downloaded = page.waitForEvent("download");
  await page.getByRole("button", { name: "备份目标 XIAO" }).click();
  const backup = JSON.parse(await readFile(await (await downloaded).path(), "utf8"));
  expect(backup.receiverAddress).toBe("2836576294");
  expect(backup.bonds).toEqual([bond]);
  expect(await page.locator("body").innerText()).not.toContain(bond.key);
});

test("pairing-fix firmware exposes physical ACK progress and exports it", async ({ page }) => {
  const errors = await install(page, { version: 3 });
  await page.getByRole("button", { name: "连接 / 授权接口" }).click();
  await expect(page.locator("#pairing-link")).toContainText("固件修订号 3");
  await expect(page.locator("#pairing-link")).toContainText("2076 / 2076 / 2076");
  const downloaded = page.waitForEvent("download");
  await page.getByRole("button", { name: "下载诊断 JSON" }).click();
  const diagnostics = JSON.parse(await readFile(await (await downloaded).path(), "utf8"));
  expect(diagnostics.groups[0].diagnostics.version).toBe(3);
  expect(diagnostics.groups[0].diagnostics.pairingLink.ackCompleted).toBe(2076);
  expect(errors).toEqual([]);
});

test("revision4 reports reconnect cause and waits for deferred key persistence", async ({ page }) => {
  const errors = await install(page, { version: 3, revision: 4 });
  await page.evaluate(() => {
    const receiver = window.__testHid.backend;
    receiver.states[0] = 3;
    receiver.deferCommit = true;
    receiver.reconnectStats = { bondCount: 1, lastOutcome: 9, restore_requests: 12,
      restore_accepted: 1, lookup_misses: 2, restore_fallbacks: 1 };
  });
  await page.getByRole("button", { name: "连接 / 授权接口" }).click();
  await expect(page.locator("#pairing-link")).toContainText("固件修订号 4");
  await expect(page.locator("#bootloader")).toBeDisabled();
  await expect(page.locator("#firmware-status")).toContainText("双击板卡 RESET");
  await expect(page.locator("#reconnect-detail")).toContainText("已保存绑定 1 条");
  await expect(page.locator("#reconnect-detail")).toContainText("已保存绑定恢复成功");
  await page.getByRole("button", { name: "保存手柄槽 1 绑定" }).click();
  await expect(page.locator("#slots .slot").first().locator(".pill")).toHaveText("已连接 · 绑定已保存");
  expect(await page.evaluate(() => window.__testHid.backend.calls.filter((item) => item.command === 0xb3).length)).toBe(1);
  expect(errors).toEqual([]);
});

test("revision5 enters UF2 mode from the page and keeps the original donor connected", async ({ page }) => {
  const errors = await install(page, { version: 3, revision: 5, donor: true });
  await expect(page.locator("#bootloader")).toBeDisabled();
  await page.getByRole("button", { name: "连接 / 授权接口" }).click();
  await expect(page.locator("#bootloader")).toBeEnabled();
  await expect(page.locator("#firmware-status")).toContainText("修订号 5");
  await page.getByRole("button", { name: "进入 UF2 刷入模式", exact: true }).click();
  await expect(page.locator("#notice")).toContainText("等待 UF2 磁盘出现");
  await expect(page.locator("#connection-status")).toContainText("0 只 XIAO，1 只原版");
  await expect(page.locator("#bootloader")).toBeDisabled();
  const state = await page.evaluate(() => ({ bootloader: window.__testHid.backend.bootloader,
    closed: window.__testHid.backend.devices.every((device) => !device.opened),
    calls: window.__testHid.backend.calls.filter((call) => call.command === 0xf9),
    donorCalls: window.__testHid.donor.calls.map((call) => call.command) }));
  expect(state.bootloader && state.closed).toBe(true);
  expect(state.calls).toHaveLength(1);
  expect(state.calls[0].payload.slice(0, 6)).toEqual([0xf9, 4, 0x55, 0x46, 0x32, 0x21]);
  expect(state.donorCalls.every((command) => command === 0xa1)).toBe(true);
  expect(errors).toEqual([]);
});

test("revision6 displays Steam handshake counters and avoids periodic B4 notification floods", async ({ page }) => {
  const errors = await install(page, { version: 3, revision: 6 });
  await page.locator("#poll").uncheck();
  await page.evaluate(() => {
    const receiver = window.__testHid.backend;
    receiver.states[0] = 2;
    receiver.hostStats = { query_busy: 73, query_ready: 2, setCommand: 0xae };
  });
  await page.getByRole("button", { name: "连接 / 授权接口" }).click();
  await expect(page.locator("#pairing-link")).toContainText("固件修订号 6");
  await expect(page.locator("#host-detail")).toContainText("73 / 2");
  await page.evaluate(() => { window.__testHid.backend.calls.length = 0; });
  await page.getByRole("button", { name: "刷新状态", exact: true }).click();
  await expect(page.locator("#refresh")).toBeEnabled();
  const calls = await page.evaluate(() => window.__testHid.backend.calls.map((call) => [call.slot, call.command]));
  expect(calls).toEqual([[1, 0xf0], [1, 0xf7], [1, 0xf8], [1, 0xfa]]);
  await expect(page.locator("#slots .slot").first().locator(".pill")).toHaveText("已连接 · 绑定已保存");
  expect(errors).toEqual([]);
});

test("a mismatched Feature response stays visible with last wireless state and recovers", async ({ page }) => {
  const errors = await install(page, { version: 3, revision: 5 });
  await page.locator("#poll").uncheck();
  await page.evaluate(() => window.__testHid.backend.states.fill(2));
  await page.getByRole("button", { name: "连接 / 授权接口" }).click();
  await expect(page.locator("#backup")).toBeEnabled();
  await page.evaluate(() => {
    const backend = window.__testHid.backend;
    backend.input(0, 2);
    backend.devices[0].overrides.push(...Array.from({ length: 50 }, () => new Uint8Array(64)));
  });
  await page.getByRole("button", { name: "刷新状态", exact: true }).click();
  await expect(page.locator("#slots .slot").first().locator(".slot-detail")).toContainText("查询异常");
  await expect(page.locator("#slots .slot").first().locator(".pill")).toHaveText("已连接 · 绑定已保存");
  await expect(page.locator("#receiver-select option")).toContainText("4/4 接口");
  await page.getByRole("button", { name: "刷新状态", exact: true }).click();
  await expect(page.locator("#slots .slot").first().locator(".slot-detail")).not.toContainText("查询异常");
  expect(errors).toEqual([]);
});

test("original identity plus journal dump migrates into XIAO and survives readback", async ({ page }) => {
  const errors = await install(page, { donor: true });
  await page.getByRole("button", { name: "连接 / 授权接口" }).click();
  await expect(page.locator("#connection-status")).toContainText("1 只原版身份来源");
  await page.getByRole("button", { name: "使用所选原版身份" }).click();
  await expect(page.locator("#import-address")).toHaveValue("a812345678");
  const journal = Buffer.alloc(1024, 255);
  journal[0] = 0xf0;
  Buffer.from(bond.controllerId, "hex").copy(journal, 1);
  Buffer.from(bond.key, "hex").copy(journal, 9);
  await page.locator("#backup-file").setInputFiles({ name: "original-journal.bin", mimeType: "application/octet-stream", buffer: journal });
  await expect(page.locator("#backup-preview")).toContainText("启动选择页：0x7400");
  await expect(page.locator("#import")).toBeEnabled();
  await page.getByRole("button", { name: "导入并应用到目标 XIAO" }).click();
  await expect(page.locator("#notice")).toContainText("Flash 回读核验通过");
  await expect(page.locator("#receiver-select")).toHaveValue("a812345678");
  const result = await page.evaluate(() => ({ address: window.__testHid.backend.address,
    bonds: window.__testHid.backend.bonds, donorCalls: window.__testHid.donor.calls.map((call) => call.command) }));
  expect(result.address).toBe("a812345678");
  expect(result.bonds).toEqual([bond]);
  expect(result.donorCalls.every((command) => command === 0xa1)).toBe(true);
  expect(await page.locator("body").innerText()).not.toContain(bond.key);
  expect(errors).toEqual([]);
});

test("JSON import is disabled while any controller is connected and rejects invalid backups", async ({ page }) => {
  await install(page);
  await page.getByRole("button", { name: "连接 / 授权接口" }).click();
  await expect(page.locator("#backup")).toBeEnabled();
  const data = { format: "sc2015-receiver-backup", version: 1, receiverAddress: "a812345678", bonds: [bond] };
  await page.locator("#backup-file").setInputFiles({ name: "bindings.json", mimeType: "application/json", buffer: Buffer.from(JSON.stringify(data)) });
  await expect(page.locator("#import")).toBeEnabled();
  await page.evaluate(() => window.__testHid.backend.input(1, 2));
  await expect(page.locator("#import")).toBeDisabled();
  await page.locator("#backup-file").setInputFiles({ name: "invalid.json", mimeType: "application/json", buffer: Buffer.from('{"format":"openpuck"}') });
  await expect(page.locator("#notice")).toContainText("不是有效的 SC2015");
  await expect(page.locator("#import")).toBeDisabled();
});

test("old diagnostics firmware and unsupported browsers explain the actual blocker", async ({ page }) => {
  await install(page, { version: 1 });
  await page.getByRole("button", { name: "连接 / 授权接口" }).click();
  await expect(page.locator("#notice")).toContainText("诊断协议 v2");
  await expect(page.locator("#pair")).toBeDisabled();
});

test("unsupported browser view and narrow layout stay usable without horizontal overflow", async ({ page }) => {
  await page.setViewportSize({ width: 390, height: 844 });
  const errors = await install(page, { unsupported: true });
  await expect(page.locator("#notice")).toContainText("不支持 WebHID");
  await expect(page.locator("#connect")).toBeDisabled();
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
  await page.screenshot({ path: "test-results/console-narrow.png", fullPage: true });
  expect(errors).toEqual([]);
});
