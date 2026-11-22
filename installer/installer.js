// SPDX-License-Identifier: GPL-2.0-only
//
// Browser installer: the README's "Installing" procedure over WebUSB.
//
//   phase 1 (Kaeru fastboot)   fastboot flash boot_a lidar-uboot.img
//                              fastboot reboot
//   phase 2 (U-Boot fastboot)  fastboot -S 64M flash system_b lidar-test-system.ext4
//                              fastboot oem run:"setenv lidar_slot b; saveenv"
//                              fastboot reboot
//
// The USB transport and the fastboot protocol come from fastboot.js (see
// vendor/SOURCE.txt). Its flashBlob() is deliberately not used: it appends
// slot suffixes on its own and only splits at the bootloader's
// max-download-size. Here the partition name is sent exactly as allow-listed
// and the image goes in 64 MiB sparse pieces, as `fastboot -S 64M` does.

import { FastbootDevice, FastbootError, TimeoutError } from "./vendor/fastboot.mjs";

/* ---------- configuration ---------- */

const REPO_URL = "https://github.com/LeagueRaINi/lidar-mainline";

// The latest published release, from the GitHub API (which allows
// cross-origin requests). Its files cannot be fetched from a page: their
// download URLs redirect to a host without CORS headers. So the page links
// to them for a normal browser download, and takes their SHA-256 from the
// API's "digest" field to check the files the user then picks.
const RELEASE_API = "https://api.github.com/repos/LeagueRaINi/lidar-mainline/releases/latest";

// The only partitions this page ever writes. system_a holds Fire OS; it stays
// off limits until the project releases it as lidar's second slot.
const ALLOW_SYSTEM_A = false;
const ALLOWED_PARTITIONS = ALLOW_SYSTEM_A ? ["boot_a", "system_b", "system_a"] : ["boot_a", "system_b"];
const SYSTEM_SLOTS = ALLOW_SYSTEM_A ? ["b", "a"] : ["b"];

const SPLIT_SIZE = 64 * 1024 * 1024;    // fastboot -S 64M
const SPARSE_BLOCK = 4096;

const IMAGES = {
  uboot: { file: "lidar-uboot.img", what: "an Android boot image", check: isBootImage },
  system: { file: "lidar-test-system.ext4", what: "a raw ext4 image", check: isExt4 },
};

/* ---------- small helpers ---------- */

const $ = (id) => document.getElementById(id);
const mib = (n) => (n / 1048576).toFixed(n < 10485760 ? 2 : 0) + " MiB";

function setPill(id, text, cls) {
  const p = $(id);
  p.textContent = text;
  p.className = "pill" + (cls ? " " + cls : "");
}

function setMsg(id, cls, text) {
  const m = $(id);
  m.hidden = !text;
  m.className = "msg " + cls;
  m.textContent = text || "";
}

function setProgress(id, fraction, text) {
  const p = $(id);
  p.hidden = false;
  p.querySelector("progress").value = fraction;
  p.querySelector("span").textContent = text;
}

async function sha256(blob) {
  const digest = await crypto.subtle.digest("SHA-256", await blob.arrayBuffer());
  return Array.from(new Uint8Array(digest), (b) => b.toString(16).padStart(2, "0")).join("");
}

async function bytesAt(blob, offset, length) {
  return new Uint8Array(await blob.slice(offset, offset + length).arrayBuffer());
}

async function isBootImage(blob) {
  return new TextDecoder().decode(await bytesAt(blob, 0, 8)) === "ANDROID!";
}

async function isExt4(blob) {
  const magic = await bytesAt(blob, 0x438, 2);      // superblock s_magic
  return magic[0] === 0x53 && magic[1] === 0xef && blob.size % SPARSE_BLOCK === 0;
}

/* ---------- steps ---------- */

const STEPS = 6;
let step = 1;
let reached = 1;
let busy = false;       // a flash is running: no navigation, warn on leaving

function showStep(n) {
  if (busy) return;
  step = n;
  reached = Math.max(reached, n);
  for (let i = 1; i <= STEPS; i++) $("step-" + i).hidden = i !== n;
  for (const b of document.querySelectorAll("nav button")) {
    const i = Number(b.dataset.step);
    b.disabled = i > reached;
    b.classList.toggle("done", i < reached);
    if (i === n) b.setAttribute("aria-current", "step"); else b.removeAttribute("aria-current");
  }
  if (n === 4) describeImage("uboot", "p1-image", "boot_a");
  if (n === 5) describeImage("system", "p2-image", systemPartition());
  updateFlashButtons();
  setArc((n - 1) / (STEPS - 1));
  scrollTo(0, 0);
}

function setArc(fraction) {
  $("logo-arc").setAttribute("stroke-dasharray", Math.round(fraction * 100) + " 100");
  $("logo").classList.toggle("done", fraction >= 1);
}

function initSteps() {
  for (const b of document.querySelectorAll("nav button")) b.addEventListener("click", () => showStep(Number(b.dataset.step)));
  for (const b of document.querySelectorAll("[data-go]")) b.addEventListener("click", () => showStep(Number(b.dataset.go)));
  $("link-prereq").href = REPO_URL + "#prerequisites";
  $("link-install").href = REPO_URL + "#installing";
  $("link-repo").href = REPO_URL;
  addEventListener("beforeunload", (e) => { if (busy) e.preventDefault(); });
  $("pre-ok").addEventListener("change", (e) => { $("next-2").disabled = !e.target.checked; });
}

/* ---------- theme (as in the web interface) ---------- */

function initTheme() {
  const THEMES = ["auto", "light", "dark"];
  const systemDark = matchMedia("(prefers-color-scheme: dark)");
  let theme = "auto";
  try { theme = localStorage.getItem("theme") || "auto"; } catch (e) { /* private mode */ }
  if (!THEMES.includes(theme)) theme = "auto";
  const apply = () => {
    const dark = theme === "dark" || (theme === "auto" && systemDark.matches);
    document.documentElement.dataset.theme = dark ? "dark" : "light";
    $("theme-label").textContent = "Theme: " + theme;
    $("theme").title = "Theme: " + theme;
  };
  $("theme").addEventListener("click", () => {
    theme = THEMES[(THEMES.indexOf(theme) + 1) % THEMES.length];
    try { localStorage.setItem("theme", theme); } catch (e) { /* private mode */ }
    apply();
  });
  systemDark.addEventListener("change", apply);
  apply();
}

/* ---------- step 1: browser ---------- */

function initBrowserCheck() {
  const usb = "usb" in navigator;
  const secure = window.isSecureContext;
  setPill("chk-usb", usb ? "available" : "missing", usb ? "on" : "warn");
  setPill("chk-ctx", secure ? "yes" : "no", secure ? "on" : "warn");
  if (!secure) setMsg("browser-msg", "bad", "Open this page over https, or from http://localhost/ when you run it yourself.");
  else if (!usb) setMsg("browser-msg", "bad", "This browser has no WebUSB. Use Chrome, Edge, Brave or Opera on a desktop computer, " +
    "or follow the README with the fastboot command-line tool instead.");
  $("next-1").disabled = !(usb && secure);
  if (navigator.userAgent.includes("Windows")) $("win-driver").open = true;
}

/* ---------- step 3: images ---------- */

// key -> { blob, name, sha256, verifiedBy, strict }; verifiedBy: what it matched ("release v0.0.1",
// "SHA256SUMS (file)"), "" if nothing; strict: it came from the one-click download
const images = {};
let release = null;     // { tag, sums: Map file -> sha256 } of the latest published release
let hosted = null;      // release.json the Pages deploy put next to the page, with the gzipped images
let userSums = null;    // { from, sums } from a SHA256SUMS the user picked (images built yourself)
const pickSeq = {};     // per image: a newer pick wins over a slower check of an older one

function parseSums(text) {
  const map = new Map();
  for (const line of text.split("\n")) {
    const m = /^([0-9a-fA-F]{64})\s+\*?(\S+)\s*$/.exec(line);
    if (m) map.set(m[2].replace(/^.*\//, ""), m[1].toLowerCase());
  }
  return map;
}

function showImage(key, pill, cls, meta) {
  setPill("img-" + key + "-pill", pill, cls);
  $("img-" + key + "-meta").textContent = meta;
  $("next-3").disabled = !(images.uboot || images.system);
}

// Checks one image (format, then checksum) and stores it, or says why not.
// A SHA256SUMS the user picked must match. The release's checksum must match
// a one-click download (strict); a file picked by hand that differs from it
// is the user's own build or another version: flashable, but not verified.
async function takeImage(key, blob, name, strict = false) {
  const spec = IMAGES[key];
  const seq = pickSeq[key] = (pickSeq[key] || 0) + 1;
  delete images[key];
  if (!blob) return showImage(key, "missing", "", "Not chosen");
  showImage(key, "checking", "", name + ", " + mib(blob.size));
  const ok = await spec.check(blob);
  if (seq !== pickSeq[key]) return;
  if (!ok) return showImage(key, "wrong file", "warn", name + " is not " + spec.what + ". Expected " + spec.file + ".");
  const hash = await sha256(blob);
  if (seq !== pickSeq[key]) return;
  const head = name + ", " + mib(blob.size), tail = ". SHA-256 " + hash;
  const keep = (verifiedBy) => { images[key] = { blob, name, sha256: hash, verifiedBy, strict }; };
  const mine = userSums && userSums.sums.get(spec.file);
  if (mine && mine !== hash)
    return showImage(key, "checksum mismatch", "warn", head + " does not match " + userSums.from + ": damaged or not that file, it will not be flashed" + tail);
  if (mine) {
    keep(userSums.from);
    return showImage(key, "verified", "on", head + ", matches " + userSums.from + tail);
  }
  const rel = release && release.sums.get(spec.file);
  if (rel && rel === hash) {
    keep("release " + release.tag);
    return showImage(key, "verified", "on", head + ", matches release " + release.tag + tail);
  }
  if (rel && strict)
    return showImage(key, "checksum mismatch", "warn", head + " does not match release " + release.tag +
      ": the download is damaged, it will not be flashed. Try again" + tail);
  keep("");
  showImage(key, "not verified", "hint", head + (rel ? ": not the file of release " + release.tag +
    " (built yourself, or another version)" : "") + tail);
}

async function recheckImages() {
  for (const key of Object.keys(IMAGES)) {
    const have = images[key];
    const input = $("file-" + key);
    if (have) await takeImage(key, have.blob, have.name, have.strict);
    else if (input.files[0]) await takeImage(key, input.files[0], input.files[0].name);
  }
}

// One image from next to the page: gzipped by the Pages deploy (if the
// server already undid the gzip, it is taken as it is)
async function fetchHosted(gz, onProgress) {
  const resp = await fetch("release/" + gz, { cache: "no-store" });
  if (!resp.ok) throw new Error(gz + ": HTTP " + resp.status);
  const total = Number(resp.headers.get("Content-Length")) || 0;
  let got = 0;
  const reader = resp.body.getReader();
  const first = await reader.read();
  const count = (chunk) => { got += chunk.length; onProgress(total ? got / total : 0, got, total); return chunk; };
  const body = new ReadableStream({
    start(c) { if (!first.done) c.enqueue(count(first.value)); else c.close(); },
    async pull(c) { const r = await reader.read(); if (r.done) c.close(); else c.enqueue(count(r.value)); },
    cancel(reason) { return reader.cancel(reason); },
  });
  const gzipped = !first.done && first.value[0] === 0x1f && first.value[1] === 0x8b;
  return await new Response(gzipped ? body.pipeThrough(new DecompressionStream("gzip")) : body).blob();
}

async function getRelease() {
  $("release-get").disabled = true;
  setMsg("img-msg", "", "");
  try {
    for (const key of Object.keys(IMAGES)) {
      const file = IMAGES[key].file;
      const blob = await fetchHosted(hosted.files[file].gz, (f, got, total) =>
        setProgress("release-prog", f, file + ": " + mib(got) + (total ? " of " + mib(total) : "")));
      await takeImage(key, blob, file, true);
    }
  } catch (e) {
    setMsg("img-msg", "bad", "Download failed (" + e.message + "). Try again, or get the files from All releases and pick them below.");
  }
  $("release-prog").hidden = true;
  $("release-get").disabled = false;
}

// The latest release: what GitHub's API says (links, checksums), and the
// copy the Pages deploy put next to the page (one-click download)
async function loadRelease() {
  $("release-more").textContent = "";
  const all = Object.assign(document.createElement("a"), { href: REPO_URL + "/releases", target: "_blank", rel: "noopener", textContent: "All releases" });
  try {
    const r = await fetch("release/release.json", { cache: "no-store" });
    if (r.ok) hosted = await r.json();
  } catch (e) { /* deployed without images */ }
  let api = null, why = "", answered = false;   // answered: GitHub said which release is the latest (or none)
  try {
    const resp = await fetch(RELEASE_API, { cache: "no-store", referrerPolicy: "no-referrer" });
    if (resp.status === 404) { why = "no release published yet"; answered = true; }
    else if (resp.headers.get("x-ratelimit-remaining") === "0")
      why = "GitHub's request limit for this network is used up until " +
        new Date(1000 * Number(resp.headers.get("x-ratelimit-reset"))).toLocaleTimeString();
    else if (!resp.ok) why = "GitHub answered " + resp.status;
    else { api = await resp.json(); answered = true; }
  } catch (e) {
    why = "no connection to GitHub";
  }
  const sums = new Map();
  let tag = "";
  if (api) {
    tag = api.tag_name;
    for (const key of Object.keys(IMAGES)) {
      const asset = (api.assets || []).find((a) => a.name === IMAGES[key].file);
      if (!asset || !String(asset.browser_download_url).startsWith(REPO_URL + "/releases/download/")) {
        why = tag + " has no usable " + IMAGES[key].file;
        api = null;
        tag = "";
        break;
      }
      $("dl-" + key).href = asset.browser_download_url;
      $("dl-" + key).textContent = "Download " + asset.name + " (" + mib(asset.size) + ")";
      const m = /^sha256:([0-9a-f]{64})$/.exec(asset.digest || "");
      if (m) sums.set(asset.name, m[1]);
    }
  }
  // the copy next to the page only if it is that release (or GitHub could
  // not be asked) and agrees with GitHub's checksums
  if (hosted && (tag ? hosted.tag === tag : !answered)) {
    for (const key of Object.keys(IMAGES)) {
      const f = hosted.files && hosted.files[IMAGES[key].file];
      const s = f && String(f.sha256).toLowerCase();
      if (!f || !/^[0-9a-f]{64}$/.test(s) || (sums.has(IMAGES[key].file) && sums.get(IMAGES[key].file) !== s)) { hosted = null; break; }
      sums.set(IMAGES[key].file, s);
    }
    if (hosted) tag = String(hosted.tag);
  } else {
    hosted = null;
  }
  if (!tag) {
    setPill("release-pill", why === "no release published yet" ? "not found" : "unavailable", "hint");
    $("release-note").textContent = "Could not get the latest release (" + why + "). Get the images from the releases page, " +
      "or build them, then pick them below.";
    $("release-more").append(all);
    return;
  }
  release = { tag, sums };
  setPill("release-pill", tag, "on");
  $("release-get").textContent = "Download and use " + tag;
  $("release-get-bar").hidden = !hosted;
  // with the one-click download there is no need for the two single links
  $("release-links").hidden = !api || !!hosted;
  $("release-note").textContent = hosted ? ""
    : sums.size ? "Download both images, then pick them below. They are checked against the release before anything is flashed."
      : "GitHub gave no checksums: compare the SHA-256 shown below with the release's SHA256SUMS yourself.";
  $("release-more").append(api ? "Published " + new Date(api.published_at).toLocaleDateString() + ". " : "",
    "Older versions and release notes: ", all, ".");
  recheckImages();
}

function initImages() {
  for (const key of Object.keys(IMAGES)) {
    $("file-" + key).addEventListener("change", (e) => {
      const f = e.target.files[0];
      takeImage(key, f, f && f.name);
    });
  }
  $("file-sums").addEventListener("change", async (e) => {
    const f = e.target.files[0];
    const parsed = f ? parseSums(await f.text()) : null;
    if (f && !parsed.size) {
      $("sums-meta").textContent = f.name + " has no SHA-256 lines.";
      return;
    }
    userSums = f ? { from: "SHA256SUMS (" + f.name + ")", sums: parsed } : null;
    $("sums-meta").textContent = f ? f.name + " loaded: the images are checked against it." : "";
    recheckImages();
  });
  $("release-get").addEventListener("click", getRelease);
  loadRelease();
}

function describeImage(key, id, partition) {
  const img = images[key];
  $(id).textContent = img
    ? img.name + " (" + mib(img.blob.size) + ", " + (img.verifiedBy ? "matches " + img.verifiedBy : "checksum not verified") + ") → " + partition
    : "No image yet: go back to Images and pick " + IMAGES[key].file + ".";
}

/* ---------- fastboot ---------- */

const fb = new FastbootDevice();
let mode = null;        // what identify() found on the connected device: "kaeru", "uboot" or null

// Tells the two fastboot implementations apart with harmless getvars. Only
// fixed, model-wide values are read and none of them is shown or logged; the
// serial number and other per-device variables are never queried.
async function identify() {
  if ((await fb.getVariable("kernel")) === "lk") {          // Amazon's LK, with Kaeru
    const product = await fb.getVariable("product");
    return product && product.toUpperCase() === "LIDAR" ? "kaeru" : null;
  }
  // lidar's U-Boot also has fixed, model-wide USB descriptors (its defconfig):
  // any other U-Boot fastboot device is refused
  const loader = await fb.getVariable("version-bootloader");
  const d = fb.device;
  return loader && loader.startsWith("U-Boot") && d.vendorId === 0x0bb4 && d.productId === 0x0c01 &&
    d.manufacturerName === "lidar-mainline" ? "uboot" : null;
}

async function getHexVar(name) {
  const v = await fb.getVariable(name);
  const n = v ? parseInt(v, 16) : NaN;
  return Number.isFinite(n) ? n : null;
}

// One sparse image that writes `data` at block `start` of a `total`-block
// partition image and skips the rest, like one piece of `fastboot -S`.
function sparsePiece(total, start, data) {
  const blocks = data.byteLength / SPARSE_BLOCK;
  const after = total - start - blocks;
  const chunks = 1 + (start ? 1 : 0) + (after ? 1 : 0);
  const out = new Uint8Array(28 + 12 * chunks + data.byteLength);
  const v = new DataView(out.buffer);
  v.setUint32(0, 0xed26ff3a, true);     // magic
  v.setUint16(4, 1, true);              // version 1.0
  v.setUint16(8, 28, true);             // file header size
  v.setUint16(10, 12, true);            // chunk header size
  v.setUint32(12, SPARSE_BLOCK, true);
  v.setUint32(16, total, true);
  v.setUint32(20, chunks, true);
  let o = 28;
  const chunk = (type, nblocks, bytes) => {
    v.setUint16(o, type, true);
    v.setUint32(o + 4, nblocks, true);
    v.setUint32(o + 8, 12 + bytes, true);
    o += 12;
  };
  if (start) chunk(0xcac3, start, 0);           // don't care
  chunk(0xcac1, blocks, data.byteLength);       // raw
  out.set(new Uint8Array(data), o);
  o += data.byteLength;
  if (after) chunk(0xcac3, after, 0);
  return out.buffer;
}

// fastboot flash <partition> <blob>. Refuses anything off the allow-list.
async function flash(partition, blob, onProgress) {
  if (!ALLOWED_PARTITIONS.includes(partition)) throw new Error("Refusing to write partition " + partition + ".");
  const limit = Math.min(SPLIT_SIZE, (await getHexVar("max-download-size")) || SPLIT_SIZE);
  if (blob.size <= limit) {
    await fb.upload(partition, await blob.arrayBuffer(), (f) => onProgress(f, "Sending"));
    onProgress(1, "Writing");
    await fb.runCommand("flash:" + partition);
    return;
  }
  if (blob.size % SPARSE_BLOCK) throw new Error("The image size is not a multiple of 4096 bytes.");
  if ((await bytesAt(blob, 0, 4)).join() === "58,255,38,237")
    throw new Error("This is already a sparse image; pick the raw image instead.");
  const total = blob.size / SPARSE_BLOCK;
  const step = Math.floor((limit - 64) / SPARSE_BLOCK) * SPARSE_BLOCK;      // 64: sparse headers
  const pieces = Math.ceil(blob.size / step);
  for (let i = 0; i < pieces; i++) {
    const offset = i * step;
    const data = await blob.slice(offset, offset + step).arrayBuffer();
    const label = "piece " + (i + 1) + " of " + pieces;
    await fb.upload(partition, sparsePiece(total, offset / SPARSE_BLOCK, data),
      (f) => onProgress((offset + f * data.byteLength) / blob.size, "Sending " + label));
    onProgress((offset + data.byteLength) / blob.size, "Writing " + label);
    await fb.runCommand("flash:" + partition);
  }
}

// The device resets as it answers; a lost reply is not a failure.
async function reboot() {
  try { await fb.runCommand("reboot"); } catch (e) { if (e instanceof FastbootError) throw e; }
  try { await fb.device.close(); } catch (e) { /* already gone */ }
  mode = null;
}

// `connecting`: the error comes from choosing and opening the device, not from talking to it.
function explain(e, connecting) {
  if (e instanceof FastbootError) return "The bootloader refused: " + (e.bootloaderMessage || e.status || "no reason given") + ".";
  if (e instanceof TimeoutError) return "No answer from the device. Unplug power, start fastboot again and reconnect.";
  if (e.name === "SecurityError" || /access denied/i.test(e.message)) return "Access denied. Close other programs using the Echo, such as fastboot. " +
    "Windows needs the WinUSB driver, Linux a udev rule (step 1).";
  if (connecting && e.name === "NotFoundError") return "No device chosen. An empty list means the Echo is not in fastboot or not seen: " +
    "check the USB, hold the button while plugging in power, and on Windows the driver (step 1).";
  if (["NetworkError", "InvalidStateError", "NotFoundError", "AbortError"].includes(e.name)) return "The USB connection was lost.";
  return e.message || String(e);
}

function confirmFlash(partition, img, warning) {
  const dlg = $("confirm");
  $("confirm-title").textContent = "Flash " + partition + "?";
  const kv = $("confirm-kv");
  kv.textContent = "";
  for (const [k, v] of [["Partition", partition], ["Image", img.name], ["Size", mib(img.blob.size)], ["SHA-256", img.sha256],
    ["Checksum", img.verifiedBy ? "matches " + img.verifiedBy : "not verified"]]) {
    kv.append(Object.assign(document.createElement("dt"), { textContent: k }), Object.assign(document.createElement("dd"), { textContent: v }));
  }
  $("confirm-warn").textContent = warning;
  return new Promise((resolve) => {
    $("confirm-yes").onclick = () => dlg.close("yes");
    $("confirm-no").onclick = () => dlg.close("no");
    dlg.onclose = () => resolve(dlg.returnValue === "yes");
    dlg.returnValue = "no";
    dlg.showModal();
  });
}

/* ---------- steps 4 and 5: the two phases ---------- */

const systemPartition = () => "system_" + $("p2-slot").value;

const PHASES = {
  p1: {
    want: "kaeru", image: "uboot", partition: () => "boot_a", next: "next-4",
    found: "Kaeru fastboot",
    wrong: "This is U-Boot's fastboot: U-Boot is already on boot_a. Skip this phase (below). " +
      "For Kaeru's fastboot, unplug power, hold Action and plug in again.",
    warn: () => "This replaces boot_a. Keep your boot_a dump: it is your way back.",
    recover: "Nothing is lost: Kaeru's fastboot does not depend on boot_a. Unplug power, hold Action, plug in and try again. " +
      "If the bootloader refuses, the Echo may not be unlocked with Kaeru.",
    done: "U-Boot is on boot_a and the Echo is rebooting. Continue with the System step.",
    async run(img, progress) {
      await flash("boot_a", img.blob, progress);
      progress(1, "Rebooting");
      await reboot();
    },
  },
  p2: {
    want: "uboot", image: "system", partition: systemPartition, next: "next-5",
    found: "U-Boot fastboot",
    wrong: "This is Kaeru's fastboot, not U-Boot's. Either Action was held, or U-Boot is not on boot_a yet (do the U-Boot step first). " +
      "Unplug power, hold Volume Up (+) and plug in again.",
    warn: () => "This erases " + systemPartition() + " and makes it the slot lidar boots from.",
    recover: "U-Boot's fastboot is still there: unplug power, hold Volume Up (+), plug in and flash again. " +
      "With an incomplete system, U-Boot opens fastboot by itself. Kaeru's fastboot (Action) also stays available.",
    done: "The system is installed and the Echo is rebooting into lidar.",
    async check(img) {
      const size = await getHexVar("partition-size:" + systemPartition());
      if (size === null) return "This U-Boot has no " + systemPartition() + " partition.";
      if (img && img.blob.size > size) return "The image (" + mib(img.blob.size) + ") is larger than " + systemPartition() + " (" + mib(size) + ").";
      return null;
    },
    async run(img, progress) {
      const slot = $("p2-slot").value;
      if (!SYSTEM_SLOTS.includes(slot)) throw new Error("Refusing slot " + slot + ".");
      await flash("system_" + slot, img.blob, progress);
      progress(1, "Selecting slot " + slot);
      await fb.runCommand("oem run:setenv lidar_slot " + slot + "; saveenv");
      progress(1, "Rebooting");
      await reboot();
    },
  },
};

function updateFlashButtons() {
  for (const [id, ph] of Object.entries(PHASES)) {
    $(id + "-flash").disabled = busy || mode !== ph.want || !images[ph.image];
    $(id + "-connect").disabled = busy;
    if (busy && mode === ph.want) setPill(id + "-pill", "flashing", "on");
  }
  $("p2-slot").disabled = busy;
}

async function connect(id) {
  const ph = PHASES[id];
  mode = null;
  setMsg(id + "-msg", "", "");
  $(id + "-connect").disabled = true;
  try {
    await fb.connect();
    const found = await identify();
    if (found === null) {
      setPill(id + "-pill", "unknown device", "warn");
      setMsg(id + "-msg", "bad", "Unknown fastboot device: neither Kaeru on an Echo Plus 2nd gen nor lidar's U-Boot. Nothing was written. " +
        "Unplug other fastboot devices and try again.");
    } else if (found !== ph.want) {
      setPill(id + "-pill", "wrong mode", "warn");
      setMsg(id + "-msg", "bad", ph.wrong);
    } else {
      const problem = ph.check ? await ph.check(images[ph.image]) : null;
      if (problem) {
        setPill(id + "-pill", "cannot flash", "warn");
        setMsg(id + "-msg", "bad", problem + " Nothing was written.");
      } else {
        mode = found;
        setPill(id + "-pill", ph.found + " connected", "on");
      }
    }
  } catch (e) {
    setPill(id + "-pill", "not connected", "warn");
    setMsg(id + "-msg", "bad", explain(e, true));
  }
  updateFlashButtons();
}

async function runPhase(id) {
  const ph = PHASES[id];
  const img = images[ph.image];
  if (mode !== ph.want || !img) return;
  // the image may have been picked or replaced after connecting
  const problem = ph.check ? await ph.check(img) : null;
  if (problem) return setMsg(id + "-msg", "bad", problem + " Nothing was written.");
  if (!(await confirmFlash(ph.partition(), img, ph.warn()))) return;
  busy = true;
  updateFlashButtons();
  setMsg(id + "-msg", "", "");
  let ok = false;
  try {
    await ph.run(img, (f, text) => {
      setProgress(id + "-prog", f, text + ", " + Math.round(f * 100) + " %");
      setArc(f);
    });
    ok = true;
  } catch (e) {
    mode = null;
    setPill(id + "-pill", "not connected", "warn");
    setMsg(id + "-msg", "bad", "Failed: " + explain(e) + "\n" + ph.recover);
  }
  busy = false;
  if (ok) {
    setMsg(id + "-msg", "ok", ph.done);
    setProgress(id + "-prog", 1, "Done");
    $(ph.next).disabled = false;
    setPill(id + "-pill", "done", "on");
  }
  updateFlashButtons();
  if (ok && id === "p2") showStep(6);
  else setArc((step - 1) / (STEPS - 1));
}

function initPhases() {
  for (const slot of SYSTEM_SLOTS) $("p2-slot").add(new Option("system_" + slot, slot));
  $("p2-slot-row").hidden = SYSTEM_SLOTS.length < 2;
  $("p2-slot").addEventListener("change", () => {
    mode = null;        // the size check belongs to the slot
    $("p2-part").textContent = systemPartition();
    describeImage("system", "p2-image", systemPartition());
    updateFlashButtons();
  });
  for (const id of Object.keys(PHASES)) {
    $(id + "-connect").addEventListener("click", () => connect(id));
    $(id + "-flash").addEventListener("click", () => runPhase(id));
  }
  $("p1-skip").addEventListener("click", () => showStep(5));
  navigator.usb.addEventListener("disconnect", () => {
    if (busy || !mode) return;      // a failing transfer reports it itself
    for (const [id, ph] of Object.entries(PHASES)) if (ph.want === mode) setPill(id + "-pill", "disconnected");
    mode = null;
    updateFlashButtons();
  });
}

/* ---------- start ---------- */

initTheme();
initSteps();
initBrowserCheck();
initImages();
if ("usb" in navigator) initPhases();
showStep(1);
