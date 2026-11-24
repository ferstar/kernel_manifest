"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { spawnSync } = require("node:child_process");

const root = path.resolve(__dirname, "..");
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "memcomp-fixture-"));
const node = path.join(tmp, "compressor");
const helper = path.join(tmp, "compressor.sh");
const endpoint = "/sys/kernel/mm/memcomp/compressor";
const report = name => console.log("PASS", name);

function installFixture() {
  const script = fs.readFileSync(path.join(root, "compressor.sh"), "utf8");
  assert.equal(script.split("node=" + endpoint).length, 2);
  // Quote the fixture path for sh; the production script has no path override.
  fs.writeFileSync(helper, script.replace("node=" + endpoint, "node='" + node.replaceAll("'", "'\\''") + "'"));
  fs.writeFileSync(path.join(tmp, "stat"), `#!/bin/sh
[ "$1" = -c ] && [ "$2" = %a ] && [ "$3" = "$FIXTURE_NODE" ] || exit 1
case "$FAULT" in stat-fail) exit 1;; root-readonly) echo 444; exit 0;; esac
${process.platform === "darwin" ? 'bits=$(/usr/bin/stat -f %Op "$3") || exit 1; printf "%o\\n" "$((0$bits & 07777))"' : 'exec /usr/bin/stat -c %a "$3"'}
`, { mode: 0o755 });
  fs.writeFileSync(path.join(tmp, "cat"), `#!/bin/sh
[ "$#" = 1 ] && [ "$1" = "$FIXTURE_NODE" ] || exit 1
value=$(/bin/cat "$1") || exit 1
case "$FAULT:$value" in
  mismatch:zstd) echo lz4; exit 0;;
  readback-error:zstd) exit 1;;
  write-error:lz4) mv "$1" "$1.before" && mkdir "$1" || exit 1;;
esac
printf '%s\\n' "$value"
`, { mode: 0o755 });
}
function reset(codec = "lz4", mode = 0o644) {
  fs.rmSync(node, { recursive: true, force: true });
  fs.writeFileSync(node, codec + "\n");
  fs.chmodSync(node, mode);
}
function call(args, fault = "") {
  const res = spawnSync("sh", [helper, ...args], {
    encoding: "utf8",
    env: { ...process.env, PATH: tmp + ":" + process.env.PATH, FIXTURE_NODE: node, FAULT: fault },
  });
  if (res.error) throw res.error;
  assert.notEqual(res.status, null);
  return { code: res.status, stdout: res.stdout, stderr: res.stderr };
}
function success(args, access, codec, fault) {
  const out = call(args, fault);
  assert.equal(out.code, 0, out.stderr);
  assert.equal(out.stdout, "access " + access + "\ncodec " + codec + "\n");
}
function rejected(args, fault) {
  const out = call(args, fault);
  assert.notEqual(out.code, 0, JSON.stringify(args));
  assert.equal(out.stdout, "");
  assert.ok(out.stderr.trim());
}
function shellCases() {
  reset(); success(["status"], "runtime", "lz4");
  for (const target of ["zstd", "lz4", "lz4"]) {
    success(["set", target], "runtime", target);
    assert.equal(fs.readFileSync(node, "utf8"), target + "\n");
  }
  report("0644, both switch directions and idempotent set");
  reset("lz4", 0o444); success(["status"], "readonly", "lz4");
  rejected(["set", "zstd"]); assert.equal(fs.readFileSync(node, "utf8"), "lz4\n");
  reset(); // Even when the process could open the file for writing, mode 0444 wins.
  fs.accessSync(node, fs.constants.W_OK);
  success(["status"], "readonly", "lz4", "root-readonly");
  rejected(["set", "zstd"], "root-readonly");
  assert.equal(fs.readFileSync(node, "utf8"), "lz4\n");
  report("v10 read-only, including a process with effective write access");
  for (const mode of [0o600, 0o664, 0o666, 0o4644]) {
    reset("lz4", mode); success(["status"], "unknown", "lz4");
    rejected(["set", "zstd"]); assert.equal(fs.readFileSync(node, "utf8"), "lz4\n");
  }
  reset(); success(["status"], "unknown", "lz4", "stat-fail");
  rejected(["set", "zstd"], "stat-fail");
  reset("other"); success(["status"], "unknown", "other"); rejected(["set", "zstd"]);
  reset("lz4\nzstd"); rejected(["status"]); rejected(["set", "zstd"]);
  fs.unlinkSync(node); rejected(["status"]); rejected(["set", "zstd"]);
  assert.equal(fs.existsSync(node), false);
  report("unexpected mode, failed stat, unknown codec, malformed read and missing endpoint");
  reset();
  const invalid = [[], ["status", "lz4"], ["set"], ["set", ""], ["set", "zstd", "extra"],
    ["set", "Zstd"], ["set", "zstd\n"], ["set", " zstd"], ["set", "lz4; id"],
    ["set", endpoint], ["set zstd"], ["--help"], ["", "lz4"]];
  for (const args of invalid) rejected(args);
  assert.equal(fs.readFileSync(node, "utf8"), "lz4\n");
  report("strict argument count, values and command allowlist");
  for (const fault of ["write-error", "mismatch", "readback-error"]) {
    reset(); rejected(["set", "zstd"], fault);
  }
  report("store rejection, divergent readback and unreadable readback");
}

// First two rows of the archived v10 ref-events.raw capture. Keep the script
// portable, including when run from the source ZIP without the device logs.
const v10Samples = [
  [279.67, 1541255168, 444774550, 549490688, 563866, 85636, 0],
  [279.87, 1532911616, 441853989, 547700736, 563866, 85636, 0],
];
const keys = ["uptime", "logical", "payload", "pool", "bg_store_ok", "direct_store_ok", "pageout_store_ok"];
const kernel = "6.1.118-android16-11-o-v20261124";
const prefix = "sh /data/adb/modules/gt5_memcomp_monitor/";
const snapshot = "snapshot.sh", status = "compressor.sh status";
const check = "vendor/install.sh --check-device";
const codecWrites = new Map([["compressor.sh set lz4", "lz4"], ["compressor.sh set zstd", "zstd"]]);
const vendorWrites = new Map();
for (const op of ["install", "restore"]) {
  for (const slot of ["_a", "_b"]) vendorWrites.set("vendor/install.sh --" + op + " " + slot, { op, slot });
}
const ok = stdout => ({ code: 0, stdout, stderr: "" });
const rawError = "OFFLINE_DIAGNOSTIC: " + "raw backend output ".repeat(15);
const fail = (stderr = rawError) => ({ code: 1, stdout: "", stderr });
const format = obj => Object.entries(obj).map(([k, v]) => k + " " + v).join("\n") + "\n";
const sizes = n => n >= 2 ** 30 ? (n / 2 ** 30).toFixed(2) + " GiB" : (n / 2 ** 20).toFixed(1) + " MiB";

function makeBackend() {
  const f = {
    // The checked v10 screenshot showed the repaired B slot and a verified backup.
    vendor: { slot: "_b", image: "fixed", backup: "verified" },
    row: v10Samples[0], errors: [], fault: "", codecFault: "", checkFault: "",
    hold: new Map(),
  };
  f.exec = async (full, options) => {
    const cmd = full.slice(prefix.length);
    try {
      assert.equal(options, "{}");
      assert.equal(full, prefix + cmd);
      if (f.hold.has(cmd)) await f.hold.get(cmd);
      if (cmd === snapshot) {
        if (f.badSnapshot) return ok("available 0\n");
        const values = Object.fromEntries(keys.map((key, i) => [key, f.row[i]]));
        return ok(format({ kernel, available: 1, policy: "applied", ...values }));
      }
      if (cmd === status) return f.codecReply || call(["status"], f.codecFault);
      if (codecWrites.has(cmd)) {
        if (f.codecFault === "wrong-reply") return ok("access runtime\ncodec lz4\n");
        const result = call(["set", codecWrites.get(cmd)], f.codecFault);
        return f.codecFault === "lost" ? { lost: true } : result;
      }
      if (cmd === check) {
        if (f.checkFault === "lost") return { lost: true };
        if (f.checkFault === "reject") return fail();
        return ok(format(f.vendor));
      }
      const write = vendorWrites.get(cmd);
      assert.ok(write, "Unexpected bridge command: " + full);
      if (write.slot !== f.vendor.slot || f.fault === "reject") return fail();
      if (f.fault === "critical") return fail("Write and rollback failed. " + f.recovery + " " + rawError);
      if (f.fault === "rolledBack") return fail("original restored and verified. " + rawError);
      if (f.fault !== "false-success") {
        assert.equal(f.vendor.image, write.op === "install" ? "stock" : "fixed");
        if (write.op === "restore") assert.equal(f.vendor.backup, "verified");
        f.vendor = { slot: write.slot, image: write.op === "install" ? "fixed" : "stock", backup: "verified" };
      }
      return f.fault === "lost" ? { lost: true } : ok("verified\n");
    } catch (error) {
      f.errors.push(error.message);
      return fail();
    }
  };
  f.pause = cmd => {
    let release;
    f.hold.set(cmd, new Promise(resolve => { release = resolve; }));
    return () => { f.hold.delete(cmd); release(); };
  };
  return f;
}

async function setup(browser, fixture) {
  const page = await browser.newPage({ viewport: { width: 390, height: 844 }, isMobile: true,
    locale: "zh-CN", colorScheme: "light", serviceWorkers: "block" });
  page.setDefaultTimeout(3000);
  page.on("pageerror", e => fixture.errors.push(e.message));
  const assets = new Map([["/index.html","text/html"], ["/style.css","text/css"], ["/app.js","text/javascript"]]);
  await page.route("**/*", async route => {
    const url = new URL(route.request().url());
    if (url.origin !== "http://offline.test" || !assets.has(url.pathname) || route.request().method() !== "GET") {
      fixture.errors.push("Unexpected request: " + route.request().url());
      return route.abort();
    }
    return route.fulfill({ contentType: assets.get(url.pathname) + "; charset=utf-8",
      body: fs.readFileSync(path.join(root, "webroot", url.pathname.slice(1))) });
  });
  await page.clock.install({ time: new Date("2026-11-25T02:00:00Z") });
  await page.clock.pauseAt(new Date("2026-11-25T02:00:01Z"));
  await page.exposeFunction("offlineCommand", fixture.exec);
  await page.addInitScript(() => {
    const pending = new Set();
    window.trace = [];
    window.ksu = { exec(cmd, options, cb) {
      window.trace.push(cmd);
      const task = window.offlineCommand(cmd, options).then(reply => {
        if (!reply.lost) window[cb](reply.code, reply.stdout, reply.stderr);
      });
      pending.add(task);
      task.finally(() => pending.delete(task));
    } };
    window.drain = async () => {
      // A settled write can synchronously enqueue another read in its promise continuation.
      do { await Promise.all([...pending]); await Promise.resolve(); } while (pending.size);
    };
  });
  await page.goto("http://offline.test/index.html");
  const id = name => page.locator("#" + name);
  const ui = {
    page, id, click: name => id(name).click(), read: name => id(name).innerText(),
    trace: () => page.evaluate(() => window.trace.splice(0)).then(rows => rows.map(cmd => cmd.slice(prefix.length))),
    flush: () => page.evaluate(() => window.drain()),
    // DOM activation of a disabled or hidden control checks the JS authorization
    // guard as well as the native button behavior.
    force: (...ids) => page.evaluate(ids => ids.forEach(id => document.getElementById(id)
      .dispatchEvent(new MouseEvent("click", { bubbles: true }))), ids),
  };
  ui.expect = async (...commands) => {
    await ui.flush();
    assert.deepEqual(await ui.trace(), commands, "bridge command sequence");
    assert.deepEqual(fixture.errors, []);
  };
  ui.homeReads = async () => {
    await ui.flush();
    assert.deepEqual((await ui.trace()).sort(), [snapshot, status].sort(), "home must read metrics and codec");
    assert.deepEqual(fixture.errors, []);
  };
  ui.refresh = async () => { await ui.click("refresh"); await ui.homeReads(); };
  ui.visible = async (name, expected = true) => {
    assert.equal(await id(name).count(), 1, "missing #" + name);
    assert.equal(await id(name).isVisible(), expected, "#" + name + " visibility");
  };
  ui.message = async (name, pattern) => {
    await ui.visible(name);
    const actual = (await ui.read(name)).trim();
    assert.match(actual, pattern);
    assert.doesNotMatch(actual, /OFFLINE_DIAGNOSTIC|raw backend output/);
  };
  return ui;
}

// Count painted text fragments by their vertical position. Element height /
// line-height also counts padding, and misses text clipped by overflow:hidden.
function measureLayout(expected) {
  const root = document.documentElement;
  const violations = [];
  const visible = el => {
    if (!el.checkVisibility({ visibilityProperty: true, opacityProperty: true })) return false;
    const closed = el.closest("details:not([open])");
    return !closed || el === closed || closed.querySelector("summary")?.contains(el);
  };
  const size = parseFloat(getComputedStyle(root).fontSize);
  if (size !== expected.font || root.clientWidth !== expected.width || root.scrollWidth > expected.width) {
    violations.push("root font/width/scrollWidth " + [size, root.clientWidth, root.scrollWidth].join("/"));
  }
  const description = el => el.id ? "#" + el.id : el.tagName + " " + el.textContent.trim().slice(0, 30);
  for (const el of document.querySelectorAll("body *")) {
    if (!visible(el)) continue;
    const rect = el.getBoundingClientRect();
    if (rect.width && (rect.left < -0.5 || rect.right > expected.width + 0.5 ||
        (el.clientWidth && el.scrollWidth > el.clientWidth + 1))) {
      violations.push(description(el) + " overflows");
    }
  }
  let maxLines = 0, blocks = 0;
  const blocksToCheck = "p,small,[role=status],[role=alert],[aria-live],#detail,#target,.modes span,.current";
  for (const el of document.querySelectorAll(blocksToCheck)) {
    if (!visible(el) || !el.textContent.trim()) continue;
    const walker = document.createTreeWalker(el, NodeFilter.SHOW_TEXT);
    const tops = [];
    while (walker.nextNode()) {
      const node = walker.currentNode;
      if (!node.textContent.trim() || !visible(node.parentElement)) continue;
      const range = document.createRange();
      range.selectNodeContents(node);
      for (const r of range.getClientRects()) {
        if (r.width && r.height && !tops.some(t => Math.abs(t - r.top) < 2)) tops.push(r.top);
      }
    }
    maxLines = Math.max(maxLines, tops.length);
    if (tops.length > 3) violations.push(description(el) + " has " + tops.length + " lines: " + el.textContent);
    if (tops.length) blocks++;
  }
  return { violations, maxLines, blocks, background: getComputedStyle(root).backgroundColor };
}

function layoutMatrix() {
  const dir = process.env.SCREENSHOT_DIR;
  if (dir) fs.mkdirSync(dir, { recursive: true });
  const errors = [], states = new Set();
  let combos = 0, blocks = 0, maxLines = 0, shots = 0;
  async function scan(ui, state) {
    const { page } = ui;
    states.add(state);
    const colors = new Set();
    for (const scheme of ["light","dark"]) {
      await page.emulateMedia({ colorScheme: scheme });
      for (const width of [320,360,390]) {
        await page.setViewportSize({width, height:844});
        for (const percent of [100,150,200]) {
          await page.evaluate(p => {
            document.documentElement.style.setProperty("font-size", p + "%", "important");
            window.scrollTo(0,0);
          }, percent);
          const r = await page.evaluate(measureLayout, { width, font: 16 * percent / 100 });
          const key = [state, width, percent, scheme].join("-");
          combos++; blocks += r.blocks; maxLines = Math.max(maxLines, r.maxLines); colors.add(r.background);
          if (!r.blocks || r.violations.length) errors.push(key + " " + (r.violations.join(", ") || "no visible text"));
          const representative = (width === 320 && percent === 200 && scheme === "dark") ||
            (width === 390 && percent === 100 && scheme === "light");
          if (dir && (state === "runtime" || representative || r.violations.length)) {
            const modal = await ui.id("confirm").isVisible();
            // A modal is fixed to the viewport. Capture that viewport directly.
            await page.screenshot({path: path.join(dir, key + ".png"), fullPage: !modal});
            shots++;
          }
        }
      }
    }
    if (colors.size !== 2) errors.push(state + " light/dark backgrounds are identical");
    await page.setViewportSize({width:390,height:844});
    await page.emulateMedia({colorScheme:"light"});
    await page.evaluate(() => document.documentElement.style.removeProperty("font-size"));
  }
  function finish() {
    console.log("Layout:", states.size, "states,", combos, "combinations,", blocks,
      "visible text checks, max", maxLines, "lines,", shots, "screenshots" + (dir ? " in " + dir : ""));
    assert.deepEqual(errors, [], "rendered layout violations");
  }
  return { scan, finish };
}

async function memoryCases(ui, f, layout) {
  await ui.homeReads();
  await ui.visible("memory"); await ui.visible("saved"); await ui.visible("codecCurrent");
  await ui.visible("vendor", false); await ui.visible("codecControls", false);
  assert.equal(await ui.read("codecCurrent"), "LZ4");
  await ui.message("codecHint", /只|查看/);
  assert.equal(await ui.read("saved"), sizes(f.row[1] - f.row[3]));
  assert.equal(await ui.id("details").evaluate(d => d instanceof HTMLDetailsElement && !d.open), true);
  await layout.scan(ui, "v10-readonly");
  await ui.force("lz4", "zstd"); await ui.expect();
  await ui.page.locator("#details > summary").click();
  for (const name of ["bg", "direct", "other", "trend"]) {
    await ui.visible(name);
    assert.equal(await ui.id(name).evaluate(el => el.closest("details")?.id), "details");
  }
  assert.deepEqual(await Promise.all(["logical","payload","pool"].map(ui.read)), f.row.slice(1,4).map(sizes));
  assert.equal(await ui.read("kernel"), kernel);
  assert.deepEqual(await Promise.all(["bg","direct","other"].map(ui.read)), ["563,866","85,636","0"]);
  assert.equal(await ui.read("bgRate"), "—");
  f.row = v10Samples[1]; await ui.page.clock.fastForward(5000); await ui.expect(snapshot);
  assert.equal(await ui.read("saved"), sizes(f.row[1] - f.row[3]));
  const pixels = () => ui.id("trend").evaluate(c => c.getContext("2d").getImageData(0,0,c.width,c.height).data.some(v => v));
  assert.ok(await pixels(), "second distinct sample must paint a trend");
  assert.equal(Number(await ui.read("bgRate")), 0);
  await layout.scan(ui, "details");
  f.row = [...f.row]; f.row[0] += 5; f.row[4] += 35;
  await ui.refresh(); assert.equal(Number(await ui.read("bgRate")), 7);
  f.badSnapshot = true; await ui.refresh();
  assert.equal(await ui.read("saved"), "—"); assert.equal(await ui.read("bgRate"), "—");
  assert.equal(await pixels(), false);
  await ui.message("error", /失败|不可用/);
  await layout.scan(ui, "statistics-unavailable");
  await ui.page.locator("#details > summary").click();
  f.badSnapshot = false; f.row = v10Samples[0];
  reset("zstd", 0o664); await ui.refresh();
  assert.equal(await ui.read("codecCurrent"), "Zstd");
  await ui.visible("codecControls", false); await ui.message("codecHint", /查看|确认/);
  await layout.scan(ui, "unknown-mode");
  fs.unlinkSync(node); await ui.refresh();
  assert.equal(await ui.read("codecCurrent"), "—");
  await ui.visible("codecControls", false); await ui.message("codecStatus", /失败|读取/);
  await layout.scan(ui, "unreadable-mode");
  assert.equal(await ui.read("saved"), sizes(f.row[1] - f.row[3]));
  reset();
  f.codecReply = ok("access runtime\ncodec unsupported\n");
  await ui.refresh(); await ui.visible("codecControls", false);
  await ui.force("zstd"); await ui.expect();
  report("v10 home, joint refresh, native details, counters/rates, drawn trend, unavailable data and unknown/malformed mode");
}

async function codecCases(ui, f, layout) {
  await ui.homeReads();
  async function selected(codec) {
    const ids = await ui.page.locator("#codecControls button:visible").evaluateAll(buttons => buttons.map(b => b.id).sort());
    assert.deepEqual(ids, ["lz4","zstd"]);
    for (const id of ids) assert.equal(await ui.id(id).getAttribute("aria-pressed"), String(id === codec), "#" + id);
    assert.equal(await ui.read("codecCurrent"), codec === "lz4" ? "LZ4" : "Zstd");
  }
  await selected("lz4"); await ui.force("lz4"); await ui.expect();
  await layout.scan(ui, "runtime");
  const zstd = "compressor.sh set zstd";
  const release = f.pause(zstd);
  await ui.click("zstd"); await ui.force("zstd", "lz4"); release(); await ui.expect(zstd);
  await selected("zstd"); await ui.message("codecStatus", /Zstd/i);
  await layout.scan(ui, "zstd-selected");
  await ui.click("lz4"); await ui.expect("compressor.sh set lz4"); await selected("lz4");

  for (const fault of ["mismatch","wrong-reply","readback-error","write-error","readonly","lost"]) {
    reset(); f.codecFault = ""; await ui.refresh(); await selected("lz4");
    if (fault === "readonly") fs.chmodSync(node, 0o444);
    f.codecFault = fault;
    await ui.click("zstd"); await ui.flush();
    if (fault === "lost") await ui.page.clock.fastForward(31000);
    await ui.flush();
    // The 5s snapshot can also fire when the clock advances through the write timeout.
    assert.deepEqual((await ui.trace()).filter(c => c !== snapshot), [zstd, status], fault);
    await ui.message("codecStatus", fault === "lost" ? /超时/ : /未确认|失败/);
    const unknown = ["readback-error","write-error"].includes(fault);
    if (unknown || fault === "readonly") {
      assert.equal(await ui.read("codecCurrent"), unknown ? "—" : "LZ4");
      await ui.visible("codecControls", false);
    } else await selected(fault === "lost" ? "zstd" : "lz4");
    if (fault === "lost" || fault === "readback-error") await layout.scan(ui, "codec-" + fault);
    assert.deepEqual(f.errors, []);
  }
  report("codec direct selection, both directions, duplicate activation, strict replies, stale permissions, rejection and lost callback readback");
}

async function checkVendor(ui) { await ui.click("check"); await ui.expect(check); }
async function vendorView(ui, data) {
  const labels = { stock: "原版", fixed: "已修复", unknown: "无法识别" };
  assert.equal(await ui.read("slot"), data.slot === "_a" ? "A 槽" : "B 槽");
  assert.equal(await ui.read("image"), labels[data.image]);
  assert.equal(await ui.read("backup"), data.backup === "verified" ? "已校验" : "未找到");
  await ui.visible("apply", data.image === "stock");
  await ui.visible("restore", data.image === "fixed" && data.backup === "verified");
}
async function modal(ui, action, slot) {
  await ui.click(action);
  const native = await ui.id("confirm").evaluate(d => ({
    modal: d instanceof HTMLDialogElement && d.matches(":modal"), focus: document.activeElement.id,
  }));
  assert.deepEqual(native, { modal: true, focus: "cancel" });
  assert.ok((await ui.read("confirm")).includes("vendor_boot" + slot));
  await ui.expect(); // opening the sheet can never dispatch a write
}

async function vendorNavigation(ui, f, layout) {
  await ui.homeReads();
  const endCheck = f.pause(check);
  await ui.click("tabVendor");
  await ui.visible("vendor"); await ui.visible("memory", false);
  await ui.visible("apply", false); await ui.visible("restore", false);
  assert.ok(await ui.id("check").isDisabled());
  await ui.force("apply","restore","proceed","check");
  await ui.visible("confirm", false);
  assert.deepEqual(await ui.trace(), [check]);
  await layout.scan(ui, "vendor-checking");
  endCheck(); await ui.expect(); await vendorView(ui, f.vendor);
  await layout.scan(ui, "vendor-v10");
  await ui.page.clock.fastForward(10000); await ui.expect(); // no background memory polling here
  for (const close of ["cancel","Escape","Enter"]) {
    await modal(ui, "restore", "_b");
    if (close === "cancel") await ui.click("cancel"); else await ui.page.keyboard.press(close);
    await ui.visible("confirm", false); await ui.force("proceed"); await ui.expect();
  }
  await ui.click("tabMemory"); await ui.homeReads();
  f.vendor = { slot:"_a", image:"stock", backup:"missing" };
  await ui.click("tabVendor"); await ui.expect(check); await vendorView(ui, f.vendor);
  await layout.scan(ui, "vendor-stock");
  for (const [image, backup] of [["fixed","missing"],["unknown","verified"],["unknown","missing"]]) {
    f.vendor = { slot:"_a", image, backup };
    await checkVendor(ui); await vendorView(ui, f.vendor);
    await ui.force("apply","restore","proceed"); await ui.visible("confirm", false); await ui.expect();
    if (backup === "verified" || image === "fixed") await layout.scan(ui, "vendor-" + image + "-" + backup);
  }
  const good = { slot:"_b", image:"fixed", backup:"verified" };
  for (const field of ["slot","image","backup"]) {
    f.vendor = { ...good, [field]:"unexpected" };
    await checkVendor(ui); await ui.visible("apply", false); await ui.visible("restore", false);
    await ui.message("vendorStatus", /失败|无法/);
    await ui.force("apply","restore","proceed"); await ui.visible("confirm", false); await ui.expect();
  }
  f.vendor = good;
  for (const fault of ["reject","lost"]) {
    f.checkFault = fault; await ui.click("check"); await ui.flush();
    if (fault === "lost") await ui.page.clock.fastForward(121000);
    await ui.expect(check); await ui.visible("apply", false); await ui.visible("restore", false);
    assert.ok(await ui.id("check").isEnabled()); await ui.message("vendorStatus", /失败|超时/);
  }
  await layout.scan(ui, "vendor-check-failed");
  f.checkFault = ""; await checkVendor(ui); await vendorView(ui, f.vendor);
  report("Vendor automatic and manual check, action visibility, slot change on return, malformed/missing backup, declined and timed-out checks, cancel/Enter/Escape");
}

async function vendorWritesCases(ui, f, layout) {
  await ui.homeReads(); await ui.click("tabVendor"); await ui.expect(check);
  // Cover every legal operation/slot pair through the actual confirmation button.
  for (const slot of ["_a","_b"]) {
    f.vendor = { slot, image:"stock", backup:"missing" }; await checkVendor(ui);
    for (const [action, op] of [["apply","install"],["restore","restore"]]) {
      await vendorView(ui, f.vendor); await modal(ui, action, slot);
      if (slot === "_a") await layout.scan(ui, "confirm-" + op);
      const cmd = "vendor/install.sh --" + op + " " + slot;
      const finishWrite = f.pause(cmd);
      await ui.click("proceed"); await ui.visible("confirm", false);
      await ui.force("proceed","apply","restore","check"); await ui.visible("confirm", false);
      await ui.visible("apply", false); await ui.visible("restore", false);
      if (action === "apply" && slot === "_a") await layout.scan(ui, "vendor-writing");
      finishWrite(); await ui.expect(cmd, check);
      await vendorView(ui, f.vendor); await ui.message("vendorStatus", /已写|成功/);
      if (slot === "_a") await layout.scan(ui, "vendor-" + op + "-success");
    }
  }
  // The backend changes slot after the dialog opens. Only the confirmed slot is sent.
  f.vendor = { slot:"_a", image:"stock", backup:"missing" }; await checkVendor(ui);
  await modal(ui, "apply", "_a");
  f.vendor = { slot:"_b", image:"fixed", backup:"verified" };
  await ui.click("proceed"); await ui.expect("vendor/install.sh --install _a", check);
  await vendorView(ui, f.vendor); await ui.message("vendorStatus", /未确认|失败/);
  await layout.scan(ui, "vendor-slot-changed");
  for (const fault of ["reject","false-success","rolledBack","critical","readback-failed"]) {
    f.fault = fault; f.recovery = "/data/local/tmp/gt5-vendor.offline-01/before.img";
    f.vendor = { slot:"_a", image:"stock", backup:"missing" }; await checkVendor(ui);
    await modal(ui, "apply", "_a");
    if (fault === "readback-failed") f.checkFault = "reject";
    await ui.click("proceed"); await ui.expect("vendor/install.sh --install _a", check);
    const recheckFailed = fault === "readback-failed";
    if (recheckFailed) {
      await ui.visible("apply", false); await ui.visible("restore", false); f.checkFault = "";
    } else await vendorView(ui, f.vendor);
    const expected = {critical:/回滚失败/,rolledBack:/回到|回滚/};
    await ui.message("vendorStatus", expected[fault] || /未确认|失败/);
    if (fault === "critical") {
      await ui.message("recoveryPath", /before\.img/); assert.equal(await ui.read("recoveryPath"), f.recovery);
    }
    await layout.scan(ui, "vendor-" + fault);
  }
  report("four allowed Vendor writes, modal focus and target, double activation, successful/rejected/divergent readback, rollback and critical diagnostics");
}

async function vendorLostWrite(ui, f, layout) {
  await ui.homeReads(); await ui.click("tabVendor"); await ui.expect(check);
  await modal(ui, "restore", "_b");
  f.fault = "lost";
  // Native button activation on a paused clock keeps the deadline exact.
  await ui.id("proceed").evaluate(button => button.click()); await ui.flush();
  assert.equal(f.vendor.image, "stock", "simulated write completed before reply loss");
  await ui.page.clock.fastForward(179999); await ui.expect("vendor/install.sh --restore _b");
  assert.ok(await ui.id("check").isDisabled(), "write receipt is still pending");
  await ui.page.clock.fastForward(1); await ui.expect(check);
  assert.ok(await ui.id("check").isEnabled());
  await vendorView(ui, f.vendor); await ui.message("vendorStatus", /超时/);
  await ui.page.clock.fastForward(180000); await ui.expect();
  await layout.scan(ui, "vendor-write-timeout");
  report("lost Vendor write callback, no write retry, read-only recovery and current slot state");
}

async function browserCases() {
  const { chromium } = require(process.env.PLAYWRIGHT_CORE || "playwright-core");
  const browser = await chromium.launch();
  const layout = layoutMatrix(), failures = [];
  try {
    for (const test of [memoryCases, codecCases, vendorNavigation, vendorWritesCases, vendorLostWrite]) {
      const fixture = makeBackend();
      reset("lz4", test === memoryCases ? 0o444 : 0o644);
      const ui = await setup(browser, fixture);
      try {
        await test(ui, fixture, layout);
        assert.deepEqual(fixture.errors, []);
      } catch (error) {
        failures.push(test.name);
        console.error("FAIL", test.name, error);
      } finally { await ui.page.close(); }
    }
    layout.finish();
    assert.deepEqual(failures, [], "failed browser suites");
  } finally { await browser.close(); }
}
(async () => {
  try {
    installFixture(); shellCases();
    if (!process.argv.includes("--shell-only")) await browserCases();
  } finally { fs.rmSync(tmp, { recursive: true, force: true }); }
})().catch(error => { console.error(error); process.exitCode = 1; });
