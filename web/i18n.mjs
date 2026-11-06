import { messages } from "./locales/en.mjs?v=22.0";

export const LANGUAGE_KEY = "sc2015.language";
export const LANGUAGES = Object.freeze(["zh-CN", "en"]);
let language = "zh-CN";
const cache = new Map();
const sources = new WeakMap();
const escape = (value) => value.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");

// Only catalogue templates are matched. Plain parameters (IDs, filenames,
// bytes, numbers) stay verbatim; ~ explicitly marks a nested UI message.
const templates = Object.entries(messages).filter(([key]) => /\{~?\w+\}/.test(key)).map(([key, english]) => {
  const parameters = [];
  let start = 0, pattern = "";
  for (const match of key.matchAll(/\{(~?)(\w+)\}/g)) {
    pattern += escape(key.slice(start, match.index)) + "([\\s\\S]*?)";
    parameters.push({ nested: Boolean(match[1]), name: match[2] });
    start = match.index + match[0].length;
  }
  pattern += escape(key.slice(start));
  return { regex: new RegExp(`^${pattern}$`), english, parameters, specificity: key.replace(/\{~?\w+\}/g, "").length };
}).sort((a, b) => b.specificity - a.specificity);

export function browserLanguage(preferred) {
  return typeof preferred === "string" && /^en(?:-|$)/i.test(preferred) ? "en" : "zh-CN";
}

export function initialLanguage(preferred, storage) {
  try {
    const saved = storage?.getItem(LANGUAGE_KEY);
    if (LANGUAGES.includes(saved)) return saved;
  } catch { /* Storage may be blocked; browser selection still works. */ }
  return browserLanguage(preferred);
}

export function setLanguage(next, storage) {
  language = LANGUAGES.includes(next) ? next : "zh-CN";
  cache.clear();
  try { storage?.setItem(LANGUAGE_KEY, language); } catch { /* In-memory switching remains available. */ }
  return language;
}
export function getLanguage() { return language; }
export function locale() { return language === "en" ? "en-US" : "zh-CN"; }

export function translate(source, depth = 0) {
  if (language !== "en" || typeof source !== "string" || depth > 8) return source;
  if (cache.has(source)) return cache.get(source);
  const text = source.trim();
  if (!/[\u3400-\u9fff]/.test(text) && !Object.hasOwn(messages, text)) return source;
  let result = Object.hasOwn(messages, text) ? messages[text] : null;
  if (result === null && text.includes("\n")) result = text.split("\n").map((line) => translate(line, depth + 1)).join("\n");
  if (result === null) {
    for (const template of templates) {
      const match = template.regex.exec(text);
      if (!match) continue;
      const values = Object.fromEntries(template.parameters.map(({ name, nested }, i) =>
        [name, nested ? translate(match[i + 1], depth + 1) : match[i + 1]]));
      result = template.english.replace(/\{(\w+)\}/g, (_, key) => values[key]);
      break;
    }
  }
  result = result === null ? source : source.slice(0, source.indexOf(text)) + result + source.slice(source.indexOf(text) + text.length);
  if (cache.size >= 4096) cache.clear();
  cache.set(source, result);
  return result;
}

// Localize presentation text only. Keep the canonical Chinese source beside
// each live node, so switching back doesn't rebuild controls, lose edits or
// reconnect HID. Protocol objects and exported diagnostics remain untouched.
export function localize(root) {
  const document = root.ownerDocument ?? root;
  const update = (node, key, current, write) => {
    const record = sources.get(node) ?? new Map();
    const old = record.get(key);
    const source = old?.rendered === current ? old.source : current;
    const rendered = translate(source);
    record.set(key, { source, rendered }); sources.set(node, record);
    if (rendered !== current) write(rendered);
  };
  const walker = document.createTreeWalker(root, 1 | 4);
  let node = walker.nextNode();
  while (node) {
    const parent = node.nodeType === 1 ? node : node.parentElement;
    if (parent && !parent.closest("script, style, textarea, [data-l10n-skip]")) {
      if (node.nodeType === 3) update(node, "text", node.data, (value) => { node.data = value; });
      else for (const attribute of ["aria-label", "title", "placeholder"]) {
        if (node.hasAttribute(attribute)) update(node, attribute, node.getAttribute(attribute), (value) => node.setAttribute(attribute, value));
      }
    }
    node = walker.nextNode();
  }
  document.documentElement.lang = language;
}
