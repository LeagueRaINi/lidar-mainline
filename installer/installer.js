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

// Where "Download images" fetches lidar-uboot.img, lidar-test-system.ext4 and
// SHA256SUMS from. Empty: no mirror yet, the button is disabled. It has to be
// a host that sends "Access-Control-Allow-Origin" (the planned
// Cloudflare-hosted mirror): GitHub release assets cannot be fetched
// cross-origin from a page, their download URLs redirect without CORS headers.
const IMAGE_BASE_URL = "";

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

// name -> { blob, name, sha256, verified }; verified: true (matches SHA256SUMS) or false (no checksum to compare)
const images = {};
let sums = null;        // Map file name -> sha256, from a SHA256SUMS file

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
async function takeImage(key, blob, name) {
  const spec = IMAGES[key];
  delete images[key];
  if (!blob) return showImage(key, "missing", "", "Not chosen");
  showImage(key, "checking", "", name + ", " + mib(blob.size));
  if (!(await spec.check(blob)))
    return showImage(key, "wrong file", "warn", name + " is not " + spec.what + ". Expected " + spec.file + ".");
  const hash = await sha256(blob);
  const want = sums && sums.get(spec.file);
  if (want && want !== hash)
    return showImage(key, "checksum mismatch", "warn", name + " does not match SHA256SUMS. It is damaged or not the published file; " +
      "it will not be flashed. SHA-256 " + hash);
  images[key] = { blob, name, sha256: hash, verified: !!want };
  showImage(key, want ? "verified" : "not verified", want ? "on" : "hint", name + ", " + mib(blob.size) + ", SHA-256 " + hash);
}

async function recheckImages() {
  for (const key of Object.keys(IMAGES)) {
    const have = images[key];
    const input = $("file-" + key);
    if (have) await takeImage(key, have.blob, have.name);
    else if (input.files[0]) await takeImage(key, input.files[0], input.files[0].name);
  }
}

async function fetchWithProgress(name, onProgress) {
  const resp = await fetch(IMAGE_BASE_URL + name, { cache: "no-store", referrerPolicy: "no-referrer" });
  if (!resp.ok) throw new Error(name + ": HTTP " + resp.status);
  const total = Number(resp.headers.get("Content-Length")) || 0;
  const parts = [];
  let got = 0;
  for (const reader = resp.body.getReader(); ;) {
    const { done, value } = await reader.read();
    if (done) break;
    parts.push(value);
    got += value.length;
    onProgress(total ? got / total : 0, got);
  }
  return new Blob(parts);
}

async function downloadImages() {
  $("mirror-btn").disabled = true;
  setMsg("img-msg", "", "");
  try {
    try {
      const resp = await fetch(IMAGE_BASE_URL + "SHA256SUMS", { cache: "no-store", referrerPolicy: "no-referrer" });
      if (resp.ok) sums = parseSums(await resp.text());
    } catch (e) { /* no checksums: the images show as "not verified" */ }
    $("sums-meta").textContent = sums ? "SHA256SUMS loaded from the mirror." : "The mirror has no SHA256SUMS; the images are not verified.";
    for (const key of Object.keys(IMAGES)) {
      const name = IMAGES[key].file;
      const blob = await fetchWithProgress(name, (f, got) => setProgress("mirror-prog", f, name + ": " + mib(got)));
      await takeImage(key, blob, name);
    }
    $("mirror-prog").hidden = true;
  } catch (e) {
    $("mirror-prog").hidden = true;
    setMsg("img-msg", "bad", "Download failed (" + e.message + "). Check the connection and try again, or pick the files by hand above.");
  }
  $("mirror-btn").disabled = false;
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
    sums = f ? parseSums(await f.text()) : null;
    if (f && !sums.size) {
      sums = null;
      $("sums-meta").textContent = f.name + " has no SHA-256 lines.";
    } else {
      $("sums-meta").textContent = f ? f.name + " loaded." : "No checksums; compare the SHA-256 values shown here yourself.";
    }
    recheckImages();
  });
  if (IMAGE_BASE_URL) {
    $("mirror-note").textContent = "From " + new URL(IMAGE_BASE_URL).host + ", checked against its SHA256SUMS.";
    $("mirror-btn").addEventListener("click", downloadImages);
  } else {
    $("mirror-note").textContent = "There is no download mirror yet. Build the images from the source tree, or get them from the project's releases page, and pick them above.";
    $("mirror-btn").disabled = true;
  }
}

function describeImage(key, id, partition) {
  const img = images[key];
  $(id).textContent = img
    ? img.name + " (" + mib(img.blob.size) + ", " + (img.verified ? "checksum verified" : "checksum not verified") + ") → " + partition
    : "No image for this phase yet: go back to Images and pick " + IMAGES[key].file + ".";
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
  if (e instanceof TimeoutError) return "The device did not answer. Unplug power, start fastboot again and reconnect.";
  if (e.name === "SecurityError" || /access denied/i.test(e.message)) return "The browser may not open the device (access denied). " +
    "Close other programs that use it, such as a running fastboot. On Windows it needs the WinUSB driver, on Linux a udev rule (step 1).";
  if (connecting && e.name === "NotFoundError") return "No device was chosen. If the list was empty, the Echo is not in fastboot or the computer does not see it: " +
    "check the USB connection, hold the button while plugging in power, and on Windows check the driver (step 1).";
  if (["NetworkError", "InvalidStateError", "NotFoundError", "AbortError"].includes(e.name)) return "The USB connection was lost.";
  return e.message || String(e);
}

function confirmFlash(partition, img, warning) {
  const dlg = $("confirm");
  $("confirm-title").textContent = "Flash " + partition + "?";
  const kv = $("confirm-kv");
  kv.textContent = "";
  for (const [k, v] of [["Partition", partition], ["Image", img.name], ["Size", mib(img.blob.size)], ["SHA-256", img.sha256],
    ["Checksum", img.verified ? "matches SHA256SUMS" : "not verified"]]) {
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
    wrong: "This is U-Boot's fastboot, so U-Boot is already on boot_a. Skip this phase (link below) and continue with the System step. " +
      "To get Kaeru's fastboot instead, unplug power, hold Action and plug in again.",
    warn: () => "This replaces the boot image on boot_a. Keep your own boot_a dump: it is the way back.",
    recover: "Nothing is lost: Kaeru's fastboot does not depend on boot_a. Unplug power, hold Action, plug in, connect and try again. " +
      "If the bootloader refuses to flash, the device may not be unlocked with Kaeru (see Before you start).",
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
    wrong: "This is Kaeru's fastboot, not U-Boot's: Action was held, or U-Boot is not on boot_a yet (then do the U-Boot step first). " +
      "Unplug power, hold Volume Up (+) and plug in again.",
    warn: () => "This erases " + systemPartition() + " and makes it the slot lidar boots from.",
    recover: "U-Boot's fastboot is still there: unplug power, hold Volume Up (+), plug in, connect and flash again. " +
      "An incomplete system slot does not boot, so U-Boot also ends up in fastboot by itself. " +
      "Kaeru's fastboot (hold Action) stays available as well.",
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
      setMsg(id + "-msg", "bad", "This fastboot device is neither Kaeru fastboot on an Echo Plus 2nd gen nor lidar's U-Boot. Nothing was written. " +
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
