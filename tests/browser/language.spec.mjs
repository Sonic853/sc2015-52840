import { test, expect } from "@playwright/test";
import { readFile } from "node:fs/promises";

const peer = (await readFile(new URL("../web/mock_hid.mjs", import.meta.url), "utf8")).replace(/^export /gm, "");

async function install(page, revision = 18) {
  await page.addInitScript({ content: `(() => {
    ${peer}
    const backend = new FakeReceiver({ version: 3, revision: ${revision} });
    const hub = new FakeHid(backend.devices);
    Object.defineProperty(navigator, 'hid', { configurable: true, value: hub });
    window.__testHid = { backend, hub };
  })();` });
  const errors = [];
  page.on("pageerror", (error) => errors.push(error.message));
  await page.goto("/");
  return errors;
}

async function untranslated(page) {
  return page.evaluate(() => {
    const result = [], walker = document.createTreeWalker(document, NodeFilter.SHOW_ELEMENT | NodeFilter.SHOW_TEXT);
    let node;
    while ((node = walker.nextNode())) {
      const parent = node.nodeType === Node.ELEMENT_NODE ? node : node.parentElement;
      if (!parent || parent.closest("script, style, [data-l10n-skip]")) continue;
      const values = node.nodeType === Node.TEXT_NODE ? [node.data] : ["aria-label", "title", "placeholder"].map((a) => node.getAttribute(a) ?? "");
      for (const value of values) if (/[\u3400-\u9fff]/.test(value)) result.push(value.trim());
    }
    return result;
  });
}

test.describe("English browser", () => {
  test.use({ locale: "en-GB" });

  test("English default covers page, live diagnostics, errors and history; toggle preserves HID and edits", async ({ page }) => {
    const errors = await install(page);
    await expect(page.locator("html")).toHaveAttribute("lang", "en");
    await expect(page).toHaveTitle("SC2015 · Receiver Console");
    await expect(page.locator("#language")).toHaveValue("en");
    expect(await untranslated(page)).toEqual([]);
    await page.getByRole("button", { name: "Connect / authorize interfaces", exact: true }).click();
    await page.locator("#poll").uncheck();
    await expect(page.locator("#connection-status")).toContainText("1 XIAO");
    await page.getByRole("button", { name: "Capture frozen queue snapshot", exact: true }).click();
    await expect(page.locator("#queue-snapshot")).toContainText("Complete frozen snapshot");
    await page.getByText("Xbox button & trackpad mapping", { exact: true }).click();
    await page.locator("#mapping-stick-deadzone").fill("4321");
    await page.locator("#mapping-button-9").selectOption("1");
    await page.locator("#input-mode").selectOption("3");
    await page.locator("#import-address").fill("a812345678");
    expect(await untranslated(page)).toEqual([]);
    const before = await page.evaluate(() => ({ calls: window.__testHid.backend.calls.length,
      opened: window.__testHid.backend.devices.map((d) => d.opened) }));
    await page.locator("#language").selectOption("zh-CN");
    await expect(page.locator("html")).toHaveAttribute("lang", "zh-CN");
    await expect(page.locator("#connection-status")).toContainText("1 只 XIAO");
    await expect(page.locator("#log")).toContainText("已连接");
    await expect(page.locator("#queue-snapshot")).toContainText("完整冻结快照");
    await page.locator("#language").selectOption("en");
    await expect(page.locator("#mapping-stick-deadzone")).toHaveValue("4321");
    await expect(page.locator("#mapping-button-9")).toHaveValue("1");
    await expect(page.locator("#input-mode")).toHaveValue("3");
    await expect(page.locator("#import-address")).toHaveValue("a812345678");
    expect(await page.evaluate(() => ({ calls: window.__testHid.backend.calls.length,
      opened: window.__testHid.backend.devices.map((d) => d.opened) }))).toEqual(before);
    await page.locator("#mapping-stick-deadzone").fill("16001");
    await page.getByRole("button", { name: "Save button mapping", exact: true }).click();
    await expect(page.locator("#notice")).toContainText("Invalid mapping");
    await page.locator("#language").selectOption("zh-CN");
    await expect(page.locator("#notice")).toContainText("按键配置、死区或斜向角度无效");
    await page.locator("#language").selectOption("en");
    expect(await untranslated(page)).toEqual([]);
    await page.setViewportSize({ width: 390, height: 844 });
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
    await expect(page.locator("#language")).toBeVisible();
    expect(errors).toEqual([]);
  });

  test("explicit Chinese selection persists after reload despite English browser", async ({ page }) => {
    await install(page);
    await page.locator("#language").selectOption("zh-CN");
    await page.reload();
    await expect(page.locator("html")).toHaveAttribute("lang", "zh-CN");
    await expect(page.locator("#language")).toHaveValue("zh-CN");
    await expect(page.getByRole("button", { name: "连接 / 授权接口", exact: true })).toBeVisible();
  });

  test("English version-gate errors update without sending unsupported mode writes", async ({ page }) => {
    await install(page, 16);
    await page.getByRole("button", { name: "Connect / authorize interfaces", exact: true }).click();
    await page.locator("#input-mode").selectOption("2");
    await page.getByRole("button", { name: "Switch input mode", exact: true }).click();
    await expect(page.locator("#notice")).toContainText("Firmware revision 16 does not support PS5 mode");
    expect(await page.evaluate(() => window.__testHid.backend.modeSaves)).toBe(0);
    await page.locator("#language").selectOption("zh-CN");
    await expect(page.locator("#notice")).toContainText("当前固件修订号 16 不支持 PS5");
  });
});

test.describe("Unsupported browser language", () => {
  test.use({ locale: "fr-FR" });
  test("falls back to Chinese and stores an English override", async ({ page }) => {
    await install(page);
    await expect(page.locator("html")).toHaveAttribute("lang", "zh-CN");
    await expect(page.locator("#language")).toHaveValue("zh-CN");
    await page.locator("#language").selectOption("en");
    await page.reload();
    await expect(page.locator("html")).toHaveAttribute("lang", "en");
    await expect(page).toHaveTitle("SC2015 · Receiver Console");
  });
});

test.describe("Chinese browser with blocked storage", () => {
  test.use({ locale: "zh-TW" });
  test("uses Chinese and can switch even when localStorage is unavailable", async ({ page }) => {
    await page.addInitScript(() => Object.defineProperty(window, "localStorage", { get() { throw new DOMException("blocked", "SecurityError"); } }));
    const errors = await install(page);
    await expect(page.locator("html")).toHaveAttribute("lang", "zh-CN");
    await page.locator("#language").selectOption("en");
    await expect(page.locator("html")).toHaveAttribute("lang", "en");
    await expect(page.getByRole("button", { name: "Connect / authorize interfaces", exact: true })).toBeVisible();
    expect(errors).toEqual([]);
  });
});
