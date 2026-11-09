#!/usr/bin/env node
// Build ./dist from ../../ui and deploy it with wrangler.
//   node deploy.mjs              build + deploy
//   node deploy.mjs --build      only build ./dist (no network)
//   node deploy.mjs --dry-run    build + `wrangler deploy --dry-run`
//   node deploy.mjs --check URL  fetch URL and print status / key headers (200 while the site is public, 302 to a login when Cloudflare Access is on, 404 while workers_dev is false)
// Any other argument is passed on to wrangler (for example --message "text").
import { cpSync, existsSync, mkdirSync, readFileSync, readdirSync, rmSync, statSync, writeFileSync } from "node:fs";
import { dirname, join, relative } from "node:path";
import { fileURLToPath } from "node:url";
import { spawnSync } from "node:child_process";

const here = dirname(fileURLToPath(import.meta.url));
const uiDir = join(here, "..", "..", "ui");
const dist = join(here, "dist");
const args = process.argv.slice(2);

// What the browser needs. Everything else in ui/ (tests, serve.py, README, package.json) stays local.
const SHIP = ["index.html", "css", "js", "wasm"];

function build() {
  rmSync(dist, { recursive: true, force: true });
  mkdirSync(dist, { recursive: true });
  for (const name of SHIP) {
    const from = join(uiDir, name);
    if (existsSync(from)) cpSync(from, join(dist, name), { recursive: true });
  }
  rmSync(join(dist, "wasm", "km_core.meta.json"), { force: true });   // build bookkeeping, not needed by the browser
  cpSync(join(here, "_headers.template"), join(dist, "_headers"));
  let files = 0, bytes = 0;
  (function walk(d) {
    for (const e of readdirSync(d)) {
      const p = join(d, e), s = statSync(p);
      if (s.isDirectory()) walk(p); else { files++; bytes += s.size; }
    }
  })(dist);
  console.log(`dist/: ${files} files, ${(bytes / 1024).toFixed(0)} KiB (from ${relative(process.cwd(), uiDir) || "."})`);
}

async function check(url) {
  const r = await fetch(url, { redirect: "manual" });
  console.log(`${r.status} ${r.statusText}  ${url}`);
  for (const h of ["location", "content-type", "cache-control", "content-security-policy", "permissions-policy", "x-frame-options", "cf-access-authenticated-user-email"]) {
    if (r.headers.has(h)) console.log(`  ${h}: ${r.headers.get(h)}`);
  }
  if (r.status === 200) console.log("  -> served without a login (expected while the site is public; if you turned Cloudflare Access on, this means it is not protecting the site).");
  else if (r.status === 302 || r.status === 301) console.log("  -> redirected (normally the Cloudflare Access login). Good.");
  else if (r.status === 403 || r.status === 401) console.log("  -> refused without a login. Good.");
  else if (r.status === 404) console.log("  -> no such address (the closed state, workers_dev=false, answers like this) or a wrong URL.");
}

const ci = args.indexOf("--check");
if (ci >= 0) {
  await check(args[ci + 1]);
} else {
  build();
  if (args.includes("--build")) process.exit(0);
  const rest = args.filter((a) => a !== "--dry-run" && a !== "--build");
  const wr = ["wrangler", "deploy", ...(args.includes("--dry-run") ? ["--dry-run"] : []), ...rest];
  const cfg = readFileSync(join(here, "wrangler.jsonc"), "utf8").replace(/\/\/.*$/gm, "");
  const open = /"workers_dev"\s*:\s*true/.test(cfg);
  if (open && !args.includes("--dry-run")) {
    console.log("NOTE: workers_dev is true -> this Worker has a public address (public by the owner's choice, see README.md).");
  }
  const r = spawnSync("npx", ["--no-install", ...wr], { cwd: here, stdio: "inherit", shell: process.platform === "win32" });
  process.exit(r.status ?? 1);
}
