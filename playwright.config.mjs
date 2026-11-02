import { defineConfig } from "@playwright/test";
import { mkdirSync } from "node:fs";
import path from "node:path";

const temporary = path.resolve("build/browser-tmp");
mkdirSync(temporary, { recursive: true });
process.env.TEMP = process.env.TMP = process.env.TMPDIR = temporary;

export default defineConfig({
  testDir: "./tests/browser",
  fullyParallel: false,
  workers: 1,
  reporter: "list",
  timeout: 30000,
  use: {
    channel: "msedge",
    headless: true,
    baseURL: "http://127.0.0.1:18765",
    viewport: { width: 1440, height: 1000 },
    trace: "retain-on-failure",
    launchOptions: { downloadsPath: path.resolve("build/browser-downloads") },
  },
  webServer: {
    command: "python -m http.server 18765 --bind 127.0.0.1 --directory web",
    url: "http://127.0.0.1:18765",
    reuseExistingServer: false,
  },
});
