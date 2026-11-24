"use strict";
const el = id => document.getElementById(id);
const set = (id, value) => { el(id).textContent = value; };
const root = "/data/adb/modules/gt5_memcomp_monitor";
const snapshotCommand = "sh " + root + "/snapshot.sh";
const vendorCommand = "sh " + root + "/vendor/install.sh ";
let callId = 0;

// KernelSU provides this bridge inside its WebView. Only fixed local commands
// below are exposed by the page.
function exec(command, timeoutMs, timeoutMessage = "读取超时，请重试。") {
  return new Promise((resolve, reject) => {
    if (!window.ksu || typeof window.ksu.exec !== "function") {
      reject(new Error("请从 KernelSU 管理器打开此页面。")); return;
    }
    const name = "memcompReply" + (++callId);
    let timer;
    function done() { clearTimeout(timer); delete window[name]; }
    window[name] = (code, stdout, stderr) => {
      done();
      if (Number(code) === 0) resolve(String(stdout || ""));
      else reject(new Error(String(stderr || stdout || "命令执行失败").trim()));
    };
    if (timeoutMs) timer = setTimeout(() => { done(); reject(new Error(timeoutMessage)); }, timeoutMs);
    try { window.ksu.exec(command, "{}", name); } catch (err) { done(); reject(err); }
  });
}
function fields(text) {
  const result = Object.create(null);
  for (const line of text.trim().split(/\r?\n/)) {
    const match = /^([A-Za-z_]+) (.+)$/.exec(line);
    if (match) result[match[1]] = match[2];
  }
  return result;
}
function count(value) {
  if (!/^\d+$/.test(value || "")) return null;
  const n = Number(value);
  return Number.isSafeInteger(n) ? n : null;
}
function size(n) {
  const gib = Math.abs(n) >= 2 ** 30;
  return (n / (gib ? 2 ** 30 : 2 ** 20)).toFixed(gib ? 2 : 1) + (gib ? " GiB" : " MiB");
}
function notice(id, message) {
  const element = el(id);
  if (element.textContent !== message) element.textContent = message;
  element.hidden = !message;
}

let page = "memory", last = null, samples = [], pending = false, timer;
const counters = { bg: "bg_store_ok", direct: "direct_store_ok", other: "pageout_store_ok" };
function resetMemory() {
  last = null; samples = [];
  set("savingLabel", "压缩省下的内存");
  for (const id of ["saved","logical","payload","pool","ratio","policy","kernel","bg","direct","other","bgRate","directRate","otherRate"]) set(id,"—");
  set("interval","等待下一次采样"); set("span","等待采样"); set("updated","等待采样");
  const c = el("trend"); c.getContext("2d").clearRect(0,0,c.width,c.height);
}
function trend() {
  const c = el("trend"), ctx = c.getContext("2d");
  ctx.clearRect(0,0,c.width,c.height);
  if (samples.length < 2) { set("span","等待下一次采样"); return; }
  const t0 = samples[0].t, t1 = samples[samples.length-1].t;
  const ys = samples.map(p => p.saved), low = Math.min(...ys), high = Math.max(...ys);
  const range = Math.max(high-low, 64 * 2**20, Math.abs(low) * 0.10), mid = (low+high)/2;
  ctx.beginPath();
  samples.forEach((p,i) => {
    const x = 3+(c.width-6)*(p.t-t0)/(t1-t0);
    const y = c.height/2 - (c.height-10)*(p.saved-mid)/range;
    if (i) ctx.lineTo(x,y); else ctx.moveTo(x,y);
  });
  ctx.lineWidth = 3; ctx.strokeStyle = getComputedStyle(document.documentElement).getPropertyValue("--accent").trim(); ctx.stroke();
  const gib = Math.max(Math.abs(low), Math.abs(high)) >= 2**30;
  const divisor = gib ? 2**30 : 2**20;
  const value = n => (n / divisor).toFixed(gib ? 2 : 1);
  const rangeText = value(low) === value(high) ? value(low) : value(low) + "–" + value(high);
  set("span", rangeText + (gib ? " GiB" : " MiB"));
}
function render(s) {
  if (s.available !== "1") throw new Error("此内核未提供可读的 memcomp 统计。");
  const logical=count(s.logical), payload=count(s.payload), pool=count(s.pool), t=Number(s.uptime);
  if ([logical,payload,pool].includes(null) || !Number.isFinite(t)) throw new Error("内核返回了无法识别的统计格式。");
  if (last && (t <= last.t || t-last.t > 20)) { last=null; samples=[]; }
  const dt=last ? t-last.t : 0;
  set("state","每 5 秒更新"); set("kernel",s.kernel || "—");
  set("savingLabel", logical < pool ? "压缩池多占的内存" : "压缩省下的内存");
  set("saved",size(Math.abs(logical-pool))); set("logical",size(logical)); set("payload",size(payload)); set("pool",size(pool));
  set("ratio",payload ? (logical/payload).toFixed(2)+"∶1" : "—");
  set("policy",({applied:"兼容配置已挂载",external:"已有其他挂载",stock:"原厂配置",unavailable:"配置不可用"})[s.policy] || "未知");
  for (const [id,key] of Object.entries(counters)) {
    const v=count(s[key]), prev=last ? count(last.s[key]) : null;
    set(id,v===null ? "—" : v.toLocaleString("zh-CN"));
    set(id+"Rate",dt && prev!==null && v!==null && v>=prev ? ((v-prev)/dt).toFixed(1) : "—");
  }
  set("interval",dt ? "采样间隔 "+dt.toFixed(1)+" 秒" : "等待下一次采样");
  samples.push({t,saved:logical-pool}); samples = samples.filter(p => t-p.t <= 60);
  trend(); last={s,t};
  if (!codecBusy && codecInfo && s.codec && s.codec !== codecInfo.codec) refreshCodec();
  set("updated","上次读取 "+new Date().toLocaleTimeString("zh-CN"));
}
function active() { return page==="memory" && !document.hidden; }
async function refresh() {
  clearTimeout(timer);
  if (!active() || pending) return;
  pending=true; el("refresh").disabled=true;
  try {
    const data=await exec(snapshotCommand,12000);
    if (active()) { render(fields(data)); notice("error",""); }
  } catch (e) { if (active()) { resetMemory(); set("state","暂无数据"); notice("error",window.ksu ? "读取失败，请点刷新。" : "请用 KernelSU 打开。"); } }
  finally { pending=false; el("refresh").disabled=false; if (active()) timer=setTimeout(refresh,5000); }
}
const compressorCommand = "sh " + root + "/compressor.sh ";
const codecLabels = new Map([["lz4", "LZ4"], ["zstd", "Zstd"]]);
const setCodecCommands = new Map([
  ["lz4", compressorCommand + "set lz4"],
  ["zstd", compressorCommand + "set zstd"]
]);
let codecInfo = null, codecBusy = false;

function showCodec(info) {
  codecInfo = info;
  const editable = info?.access === "runtime";
  set("codecCurrent", info ? (codecLabels.get(info.codec) || info.codec) : "—");
  el("codecControls").hidden = !editable;
  for (const id of codecLabels.keys()) {
    const selected = info?.codec === id;
    el(id).setAttribute("aria-pressed", String(selected));
    el(id).disabled = !editable || codecBusy || selected;
  }
  const hint = {readonly: "当前内核只支持查看。", unknown: "此模式仅可查看。",
    runtime: "新数据用此模式，重启后恢复默认。"};
  notice("codecHint", info ? hint[info.access] : "");
}

async function readCodec() {
  const info = fields(await exec(compressorCommand + "status", 12000));
  if (!["runtime", "readonly", "unknown"].includes(info.access) ||
      !/^[a-zA-Z0-9_-]+$/.test(info.codec || "") ||
      (info.access === "runtime" && !codecLabels.has(info.codec))) {
    throw new Error("无法读取模式");
  }
  return info;
}

async function refreshCodec() {
  if (codecBusy) return;
  codecBusy = true;
  showCodec(codecInfo);
  try {
    const actual = await readCodec();
    codecBusy = false;
    showCodec(actual);
    notice("codecStatus", "");
  } catch {
    codecBusy = false;
    showCodec(null);
    notice("codecStatus", "模式读取失败，请点刷新。");
  }
}

async function applyCodec(target) {
  if (codecBusy || codecInfo?.access !== "runtime" || codecInfo.codec === target ||
      !setCodecCommands.has(target)) return;
  codecBusy = true;
  showCodec(codecInfo);
  notice("codecStatus", "正在切换…");
  let actual = null;
  try {
    const result = fields(await exec(setCodecCommands.get(target), 30000, "timeout"));
    if (result.access !== "runtime" || result.codec !== target) throw new Error("回读不符");
    actual = result;
    notice("codecStatus", "已切换为 " + codecLabels.get(target) + "。");
  } catch (error) {
    try { actual = await readCodec(); } catch { /* The mode is unknown until a fresh read. */ }
    notice("codecStatus", error.message === "timeout" ? "请求超时，请核对当前模式。" : "切换未确认，请核对当前模式。");
  } finally {
    codecBusy = false;
    showCodec(actual);
  }
}

let info = null, busy = false, choice = null;
const imageLabels = {stock:"原版", fixed:"已修复", unknown:"无法识别"};

function allowed(kind) {
  if (!info) return false;
  if (kind === "install") return info.image === "stock";
  return kind === "restore" && info.image === "fixed" && info.backup === "verified";
}

function showVendor(value) {
  info = value;
  set("slot", info ? info.slot.slice(1).toUpperCase() + " 槽" : "—");
  set("image", info ? imageLabels[info.image] : "待检查");
  set("backup", info ? (info.backup === "verified" ? "已校验" : "未找到") : "—");
  let hint = "";
  if (info?.image === "unknown") hint = "镜像不匹配，无法修复。";
  if (info?.image === "fixed" && info.backup === "missing") hint = "缺少原版备份，无法还原。";
  notice("vendorHint", hint);
  for (const [id, kind] of [["apply","install"], ["restore","restore"]]) {
    el(id).hidden = !allowed(kind);
    el(id).disabled = busy || !allowed(kind);
  }
  el("check").disabled = busy;
}

async function readVendor() {
  const value = fields(await exec(vendorCommand + "--check-device", 120000));
  if (!["_a","_b"].includes(value.slot) || !Object.hasOwn(imageLabels, value.image) ||
      !["missing","verified"].includes(value.backup)) throw new Error("检查失败");
  return value;
}

async function check() {
  if (busy) return;
  busy = true;
  showVendor(null);
  notice("vendorStatus", "正在检查驱动和备份…");
  let actual = null;
  try {
    actual = await readVendor();
    notice("vendorStatus", "检查完成，驱动" + (actual.image === "stock" ? "为原版" : imageLabels[actual.image]) + "。");
  } catch { notice("vendorStatus", "检查失败，请点重新检查。"); }
  finally { busy = false; showVendor(actual); }
}

function confirmAction(kind) {
  if (busy || !allowed(kind) || el("confirm").open) return;
  choice = {kind, slot:info.slot};
  set("question", kind === "install" ? "刷入修复版？" : "还原原版驱动？");
  set("target", "vendor_boot" + choice.slot);
  set("detail", kind === "install" ? "先备份，再写入。重启后生效。" : "将写回原版备份。重启后生效。");
  el("confirm").showModal();
}

const writeCommands = {
  install: {_a:"--install _a", _b:"--install _b"},
  restore: {_a:"--restore _a", _b:"--restore _b"}
};

async function write() {
  const request = choice;
  if (!el("confirm").open || !request || busy || !allowed(request.kind) ||
      info.slot !== request.slot) return;
  choice = null;
  el("confirm").close();
  busy = true;
  showVendor(null);
  notice("recoveryPath", "");
  notice("vendorStatus", "正在写入并校验，请稍候。");
  let result;
  try {
    // A missing bridge reply must not strand the page or repeat a partition write.
    await exec(vendorCommand + writeCommands[request.kind][request.slot], 180000, "timeout");
    result = "ok";
  } catch (e) {
    if (e.message.includes("Write and rollback failed")) {
      result = "critical";
      const path = e.message.match(/\/data\/local\/tmp\/gt5-vendor\.[\w-]+\/before\.img/);
      if (path) notice("recoveryPath", path[0]);
    } else if (e.message === "timeout") result = "timeout";
    else if (e.message.includes("restored and verified")) result = "rolledBack";
    else result = "failed";
  }
  let actual = null;
  try { actual = await readVendor(); } catch { /* Display a retry action. */ }
  busy = false;
  showVendor(actual);
  const expected = request.kind === "install" ? "fixed" : "stock";
  let message = "写入未确认，请重新检查。";
  if (result === "timeout") message = "写入回执超时，请核对状态。";
  else if (result === "critical") message = "写入和回滚失败，请勿重启。";
  else if (result === "rolledBack") message = "写入失败，已回到操作前状态。";
  else if (result === "ok" && actual?.slot === request.slot && actual.image === expected) {
    message = request.kind === "install" ? "已写入修复版。重启后生效。" : "已写回原版。重启后生效。";
  }
  notice("vendorStatus", message);
}

function refreshHome() { refresh(); refreshCodec(); }
function switchPage(next) {
  if (next === page) return;
  page = next;
  clearTimeout(timer);
  for (const [id, value] of [["tabMemory","memory"], ["tabVendor","vendor"]]) {
    el(id).setAttribute("aria-pressed", String(next === value));
    el(value).hidden = next !== value;
  }
  if (active()) refreshHome();
  if (next === "vendor") check();
}

el("tabMemory").onclick = () => switchPage("memory");
el("tabVendor").onclick = () => switchPage("vendor");
el("refresh").onclick = refreshHome;
for (const id of codecLabels.keys()) el(id).onclick = () => applyCodec(id);
el("check").onclick = check;
el("apply").onclick = () => confirmAction("install");
el("restore").onclick = () => confirmAction("restore");
el("cancel").onclick = () => el("confirm").close();
el("confirm").onclose = () => { choice = null; };
el("proceed").onclick = write;
document.addEventListener("visibilitychange", () => {
  clearTimeout(timer);
  if (active()) refreshHome();
});
showVendor(null);
showCodec(null);
refreshHome();
