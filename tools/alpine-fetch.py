#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Install Alpine aarch64 packages (with dependencies) into a rootfs directory
without running apk, which cannot execute on the x86 build host.

usage: alpine-fetch.py ROOTFS PKG [PKG...]
       alpine-fetch.py --update [PKG...]

The Alpine branch is read from ROOTFS/etc/alpine-release. Every package must
be in tools/alpine.lock with the version Alpine serves and the SHA-256 of its
.apk, so that one commit always builds the same system; anything else stops
the build. What was installed is appended to ROOTFS/usr/share/lidar/packages
(name, version, licence).

--update rewrites the lock for the current Alpine stable release: the
minirootfs, every package already in the lock and the ones named, with their
dependencies. Alpine keeps only the newest build of a package, so a lock
goes stale when Alpine updates one of them; update it and commit the result.
To drop packages nothing needs any more, delete their lines first.
"""
import hashlib
import io
import os
import re
import sys
import tarfile
import urllib.request

MIRROR = "https://dl-cdn.alpinelinux.org/alpine"
REPOS = ("main", "community")
LOCK = os.path.join(os.path.dirname(os.path.abspath(__file__)), "alpine.lock")
UPDATE_HINT = "bring tools/alpine.lock up to date with: alpine-fetch.py --update"


def fetch(url: str) -> bytes:
    with urllib.request.urlopen(url, timeout=60) as r:
        return r.read()


def branch_of(ver: str) -> str:
    return "v" + ".".join(ver.split(".")[:2])


def load_index(branch: str):
    pkgs, provides = {}, {}
    for repo in REPOS:
        base = f"{MIRROR}/{branch}/{repo}/aarch64"
        raw = fetch(f"{base}/APKINDEX.tar.gz")
        with tarfile.open(fileobj=io.BytesIO(raw)) as t:
            text = t.extractfile("APKINDEX").read().decode()
        for block in text.split("\n\n"):
            f = {}
            for line in block.splitlines():
                if len(line) > 2 and line[1] == ":":
                    f.setdefault(line[0], line[2:])
            if "P" not in f:
                continue
            f["url"] = f"{base}/{f['P']}-{f['V']}.apk"
            pkgs[f["P"]] = f
            for p in f.get("p", "").split():
                provides.setdefault(p.split("=")[0], f["P"])
    return pkgs, provides


def resolve(names, pkgs, provides):
    want, todo = [], list(names)
    while todo:
        dep = todo.pop()
        if dep.startswith("!"):
            continue
        dep = dep.split(">")[0].split("<")[0].split("=")[0].split("~")[0]
        name = dep if dep in pkgs else provides.get(dep)
        if name is None:
            raise SystemExit(f"unresolvable dependency: {dep}")
        if name in want:
            continue
        want.append(name)
        todo.extend(pkgs[name].get("D", "").split())
    return want


def read_lock():
    """-> (release, minirootfs sha256), {package: (version, sha256)}"""
    release, locked = None, {}
    with open(LOCK) as f:
        for line in f:
            w = line.split()
            if not w or w[0].startswith("#"):
                continue
            if w[0] == "release":
                release = (w[1], w[2])
            else:
                locked[w[0]] = (w[1], w[2])
    if release is None:
        raise SystemExit(f"{LOCK}: no release line")
    return release, locked


def update(names):
    listing = fetch(f"{MIRROR}/latest-stable/releases/aarch64/latest-releases.yaml").decode()
    ver = re.search(r"alpine-minirootfs-(\S+)-aarch64\.tar\.gz", listing).group(1)
    branch = branch_of(ver)
    rootfs = fetch(f"{MIRROR}/{branch}/releases/aarch64/alpine-minirootfs-{ver}-aarch64.tar.gz")
    old_release, locked = read_lock() if os.path.exists(LOCK) else ((None, None), {})
    # a digest is only reused within one branch: Alpine rebuilds every
    # package for a new branch, even at the same version
    same_branch = old_release[0] is not None and branch_of(old_release[0]) == branch
    pkgs, provides = load_index(branch)
    lines = [
        "# SPDX-License-Identifier: GPL-2.0-only",
        "# The Alpine release (minirootfs) and packages the system image and the",
        "# build sysroot are made of: name, version, SHA-256. Checked by",
        "# tools/alpine-fetch.py and tools/mk-testimg.sh; written by",
        "# alpine-fetch.py --update, not by hand.",
        f"release {ver} {hashlib.sha256(rootfs).hexdigest()}",
    ]
    for name in sorted(resolve(list(locked) + names, pkgs, provides)):
        v = pkgs[name]["V"]
        if same_branch and locked.get(name, ("",))[0] == v:
            digest = locked[name][1]
        else:
            digest = hashlib.sha256(fetch(pkgs[name]["url"])).hexdigest()
            print(f"{name}: {locked[name][0] if name in locked else 'new'} -> {v}")
        lines.append(f"{name} {v} {digest}")
    with open(LOCK, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print(f"{LOCK}: Alpine {ver}, {len(lines) - 6} packages")


def install(root, names):
    ver = open(f"{root}/etc/alpine-release").read().strip()
    release, locked = read_lock()
    if ver != release[0]:
        raise SystemExit(f"{root} is Alpine {ver}, tools/alpine.lock is for {release[0]}")
    pkgs, provides = load_index(branch_of(ver))
    want = resolve(names, pkgs, provides)
    stale = [f"  {n}: Alpine has {pkgs[n]['V']}, the lock {locked[n][0] if n in locked else 'nothing'}"
             for n in want if locked.get(n, ("",))[0] != pkgs[n]["V"]]
    if stale:
        raise SystemExit("\n".join(["packages differ from tools/alpine.lock:"] + stale + [UPDATE_HINT]))
    os.makedirs(f"{root}/usr/share/lidar", exist_ok=True)
    for name in want:
        data = fetch(pkgs[name]["url"])
        if hashlib.sha256(data).hexdigest() != locked[name][1]:
            raise SystemExit(f"{name} {pkgs[name]['V']}: not the file in tools/alpine.lock (SHA-256 differs)")
        # an .apk is concatenated gzip streams: signature, control, data
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as t:
            members = [m for m in t.getmembers() if not m.name.startswith(".")]
            t.extractall(root, members=members, filter="fully_trusted")
        with open(f"{root}/usr/share/lidar/packages", "a") as m:
            m.write(f"{name} {pkgs[name]['V']} {pkgs[name].get('L', '?')}\n")
        print("installed", name)


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "--update":
        update(sys.argv[2:])
    elif len(sys.argv) >= 3:
        install(sys.argv[1], sys.argv[2:])
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
