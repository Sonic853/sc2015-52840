import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { LANGUAGE_KEY, browserLanguage, initialLanguage, setLanguage, translate } from "../web/i18n.mjs";
import { messages } from "../web/locales/en.mjs";

test("browser primary language supports English variants and otherwise falls back to Chinese", () => {
  for (const value of ["en", "en-US", "en-GB", "EN-au"]) assert.equal(browserLanguage(value), "en");
  for (const value of ["zh", "zh-CN", "zh-TW", "fr-FR", "ja-JP", "enochian", "", undefined]) assert.equal(browserLanguage(value), "zh-CN");
});

test("saved explicit language wins; invalid or unavailable storage uses browser defaults", () => {
  const data = new Map(), storage = { getItem: (key) => data.get(key), setItem: (key, value) => data.set(key, value) };
  assert.equal(initialLanguage("en-GB", storage), "en");
  assert.equal(data.size, 0); // detecting a default must not turn it into a saved preference
  setLanguage("zh-CN", storage);
  assert.equal(data.get(LANGUAGE_KEY), "zh-CN");
  assert.equal(initialLanguage("en-US", storage), "zh-CN");
  setLanguage("en", storage);
  assert.equal(initialLanguage("fr-FR", storage), "en");
  data.set(LANGUAGE_KEY, "fr"); assert.equal(initialLanguage("en-US", storage), "en");
  const blocked = { getItem() { throw new Error("blocked"); }, setItem() { throw new Error("blocked"); } };
  assert.equal(initialLanguage("fr-FR", blocked), "zh-CN");
  assert.doesNotThrow(() => setLanguage("en", blocked));
  assert.equal(translate("保存绑定"), "Save bond");
  setLanguage("zh-CN");
});

test("localization handles nested messages and multiline diagnostics without translating data", () => {
  setLanguage("en");
  assert.equal(translate("槽 2：USB 请求超时；请重新连接接收器；已暂停此接口查询，请重新连接 / 授权接口"),
    "Slot 2: USB request timed out; reconnect the receiver; queries suspended for this interface. Reconnect / authorize it");
  assert.equal(translate("文件：保存绑定.json"), "File: 保存绑定.json");
  assert.equal(translate("序号 17\n0100013c"), "Sequence 17\n0100013c");
  assert.equal(translate("完整冻结快照 · 主机采集时间 2026-11-04\n快照 9 · MCU clock_us 123 · 协议时钟可用"),
    "Complete frozen snapshot · Host capture time 2026-11-04\nSnapshot 9 · MCU clock_us 123 · Protocol clock available");
  assert.equal(translate("当前：PS5 / DualSense（PC） · 输入来源：槽 1 · 默认重新上电回到 Steam"),
    "Current: PS5 / DualSense (PC) · Input source: Slot 1 · Returns to Steam on next power-up by default");
  assert.equal(translate("  保存绑定  "), "  Save bond  ");
  assert.equal(translate("unexpected browser error: <tag>"), "unexpected browser error: <tag>");
  assert.equal(translate("未收录的文案"), "未收录的文案");
  setLanguage("zh-CN"); assert.equal(translate("保存绑定"), "保存绑定");
});

test("English catalogue preserves placeholders and covers static protocol/manager messages", async () => {
  const placeholders = (value) => [...new Set([...value.matchAll(/\{~?(\w+)\}/g)].map((m) => m[1]))].sort();
  for (const [source, target] of Object.entries(messages)) {
    assert.deepEqual(placeholders(target), placeholders(source), source);
    assert.ok(!/[\u3400-\u9fff]/.test(target), source);
  }
  setLanguage("en");
  for (const file of ["protocol.mjs", "manager.mjs"]) {
    const source = await readFile(new URL(`../web/${file}`, import.meta.url), "utf8");
    for (const match of source.matchAll(/"(?:[^"\\\r\n]|\\.)*"/g)) {
      const literal = JSON.parse(match[0]);
      if (/[\u3400-\u9fff]/.test(literal)) assert.notEqual(translate(literal), literal, `${file}: ${literal}`);
    }
  }
  setLanguage("zh-CN");
});
