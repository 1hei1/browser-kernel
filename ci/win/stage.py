#!/usr/bin/env python3
"""MZ kernel Windows CI helper.

Subcommands (all run on a GitHub-hosted windows runner):
  sync      fetch Chromium <tag> with depot_tools (shallow, no history) + runhooks
  prepare   write out/Default/args.gn, run the MZ driver (apply + deploy), gn gen
  build     run ninja until it finishes or the time budget for this job runs out
  package   turn out/Default/chrome.7z into the mz-browser-kernel zip

Layout under %MZ_WORK% (chosen by the workflow):
  depot_tools/        chromium/.gclient   chromium/src/   dist/
"""
from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import signal
import subprocess
import sys
import time
import zipfile
from pathlib import Path

KERNEL = Path(__file__).resolve().parents[2]
WORK = Path(os.environ.get("MZ_WORK", "C:/mzbuild"))
DEPOT = WORK / "depot_tools"
CHROMIUM = WORK / "chromium"
SRC = CHROMIUM / "src"
OUT_REL = "out/Default"
OUT = SRC / OUT_REL
TAG = os.environ.get("CHROMIUM_TAG", "157.0.8103.51")
BUILD_TARGETS = os.environ.get("MZ_TARGETS", "mini_installer").split()


def log(msg: str) -> None:
    print(f"::notice::{msg}" if os.environ.get("GITHUB_ACTIONS") else msg, flush=True)


def env_with_depot() -> dict:
    env = dict(os.environ)
    env["PATH"] = str(DEPOT) + os.pathsep + env["PATH"]
    env["DEPOT_TOOLS_WIN_TOOLCHAIN"] = "0"
    env["DEPOT_TOOLS_UPDATE"] = "0"
    env["DEPOT_TOOLS_METRICS"] = "0"
    env.setdefault("GYP_MSVS_VERSION", "2022")
    return env


def run(cmd, cwd=None, env=None, check=True):
    print("+", " ".join(map(str, cmd)), flush=True)
    return subprocess.run([str(c) for c in cmd], cwd=cwd, env=env or env_with_depot(),
                          check=check, shell=False)


def set_output(key: str, value: str) -> None:
    out = os.environ.get("GITHUB_OUTPUT")
    if out:
        with open(out, "a", encoding="utf-8") as f:
            f.write(f"{key}={value}\n")
    print(f"[output] {key}={value}")


def summary(text: str) -> None:
    path = os.environ.get("GITHUB_STEP_SUMMARY")
    if path:
        with open(path, "a", encoding="utf-8") as f:
            f.write(text + "\n")
    print(text)


# ---------------------------------------------------------------- sync
def cmd_sync(_args) -> None:
    WORK.mkdir(parents=True, exist_ok=True)
    if not DEPOT.exists():
        run(["git", "clone", "--depth", "1",
             "https://chromium.googlesource.com/chromium/tools/depot_tools.git", DEPOT])
    env = env_with_depot()
    env["DEPOT_TOOLS_UPDATE"] = "1"  # first call bootstraps python/git inside depot_tools
    run(["cmd", "/c", "gclient.bat", "--version"], env=env, check=False)

    CHROMIUM.mkdir(parents=True, exist_ok=True)
    (CHROMIUM / ".gclient").write_text(
        "solutions = [{\n"
        "  'name': 'src',\n"
        "  'url': 'https://chromium.googlesource.com/chromium/src.git',\n"
        "  'managed': False,\n"
        "  'custom_deps': {},\n"
        "  'custom_vars': {'checkout_pgo_profiles': False},\n"
        "}]\n"
        "target_os = ['win']\n",
        encoding="utf-8",
    )
    if not (SRC / ".git").exists():
        run(["git", "init", "-q", SRC])
        run(["git", "-C", SRC, "remote", "add", "origin",
             "https://chromium.googlesource.com/chromium/src.git"])
    run(["git", "-C", SRC, "fetch", "--depth", "1", "origin",
         f"+refs/tags/{TAG}:refs/tags/{TAG}"])
    run(["git", "-C", SRC, "checkout", "-q", "-f", f"tags/{TAG}"])
    t0 = time.time()
    run(["cmd", "/c", "gclient.bat", "sync", "--no-history", "--shallow", "--nohooks",
         "-D", "--force", "-j", "16"], cwd=CHROMIUM)
    log(f"gclient sync took {(time.time() - t0) / 60:.1f} min")
    t0 = time.time()
    run(["cmd", "/c", "gclient.bat", "runhooks"], cwd=CHROMIUM)
    log(f"gclient runhooks took {(time.time() - t0) / 60:.1f} min")
    version = (SRC / "chrome" / "VERSION").read_text().split()
    log("chrome/VERSION: " + " ".join(version))


# ---------------------------------------------------------------- prepare
def cmd_prepare(_args) -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(KERNEL / "ci" / "win" / "args.gn", OUT / "args.gn")

    env = env_with_depot()
    env["MZ_CHROMIUM_ROOT"] = str(SRC)
    env["MZ_OUT_DIR"] = OUT_REL
    # The driver applies patches with GNU patch; Git for Windows ships one.
    git_usr_bin = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "Git" / "usr" / "bin"
    env["PATH"] = env["PATH"] + os.pathsep + str(git_usr_bin)
    failures = WORK / "driver-failures.json"
    run(["uv", "run", "python", "-m", "driver", "validate"], cwd=KERNEL, env=env)
    r = run(["uv", "run", "python", "-m", "driver", "--out-dir", OUT_REL,
             "--failures-json", failures, "apply-and-prepare"], cwd=KERNEL, env=env, check=False)
    if r.returncode != 0:
        retry_failed_patches(failures, env)
    print("---- final args.gn ----")
    print((OUT / "args.gn").read_text(encoding="utf-8"))
    run(["cmd", "/c", "gn.bat", "gen", OUT_REL], cwd=SRC)
    run(["cmd", "/c", "gn.bat", "args", OUT_REL, "--list", "--short", "--overrides-only"],
        cwd=SRC, check=False)
    fill_missing_brand_sources()


def fill_missing_brand_sources() -> None:
    """The branding overlay sets branding_path_product = "mz", so GN asks for
    files like chrome/installer/setup/mz_behaviors.cc that the overlay never
    ships (upstream's tree evidently had them). Dry-run ninja, and for each
    missing mz_* source copy its chromium_* sibling. Anything else fails here,
    seconds after gn gen, instead of hours into the build."""
    import re
    pat = re.compile(r"'([^']+)', needed by '[^']+', missing and no known rule")
    for _ in range(20):
        r = subprocess.run([str(ninja_exe()), "-C", OUT_REL, "-n", "mini_installer"],
                           cwd=SRC, capture_output=True, text=True, errors="replace")
        m = pat.search(r.stdout + r.stderr)
        if not m:
            if r.returncode != 0:
                print((r.stdout + r.stderr)[-4000:])
                raise SystemExit("ninja dry run failed")
            log("ninja dry run OK: no missing inputs")
            return
        missing = (OUT / m.group(1)).resolve()
        sibling = missing.with_name(missing.name.replace("mz_", "chromium_", 1))
        if "mz_" not in missing.name or not sibling.exists():
            raise SystemExit(f"missing input with no chromium_ sibling: {missing}")
        shutil.copyfile(sibling, missing)
        log(f"created {missing.relative_to(SRC)} from {sibling.name}")
    raise SystemExit("too many missing inputs")


def retry_failed_patches(failures_json: Path, env: dict) -> None:
    """Git for Windows' GNU patch rejects a few hunks that apply cleanly with git apply
    (seen: fingerprint/059 hunk 5 on 157.0.8103.51). For each failed plain .patch:
    restore its target files from git, drop .rej files, re-apply with git apply,
    then run the driver's own hunk verification. Anything else stays a failure."""
    import json
    sys.path.insert(0, str(KERNEL))
    from driver.patching import parse_hunks, read_normalized_patch, verify_hunks_applied

    items = json.loads(failures_json.read_text(encoding="utf-8"))
    still_failed = []
    for item in items:
        label = item["label"]
        parts = label.split("/")
        if len(parts) != 3 or parts[0] != "overlay" or not label.endswith(".patch"):
            still_failed.append(label)
            continue
        patch_file = KERNEL / "overlay" / parts[1] / "patches" / parts[2]
        data = read_normalized_patch(patch_file)
        hunks = parse_hunks(data.decode("utf-8"))
        targets = sorted({h.target for h in hunks if h.target})
        shared = [t for t in targets
                  if any(t in q.read_text(encoding="utf-8", errors="replace")
                         for q in (KERNEL / "overlay").glob("*/patches/*.patch*") if q != patch_file)]
        if shared:
            print(f"{label}: targets also patched elsewhere, not retrying: {shared}")
            still_failed.append(label)
            continue
        for t in targets:
            run(["git", "checkout", "--", t], cwd=SRC, check=False)
            rej = SRC / (t + ".rej")
            if rej.exists():
                rej.unlink()
        tmp = WORK / "retry.patch"
        tmp.write_bytes(data)
        r = subprocess.run(["git", "apply", "--verbose", "--whitespace=nowarn", str(tmp)],
                           cwd=SRC, env=env)
        problems = verify_hunks_applied(SRC, hunks) if r.returncode == 0 else ["git apply failed"]
        if problems:
            print("\n".join(problems))
            still_failed.append(label)
        else:
            log(f"{label}: GNU patch failed, applied cleanly with git apply")
    if still_failed:
        raise SystemExit("patches still failing: " + ", ".join(still_failed))


# ---------------------------------------------------------------- build
def ninja_exe() -> Path:
    for p in (SRC / "third_party" / "ninja" / "ninja.exe", DEPOT / "ninja.exe"):
        if p.exists():
            return p
    raise SystemExit("ninja.exe not found")


def cmd_build(args) -> None:
    job_start = float(os.environ.get("MZ_JOB_START", time.time()))
    deadline = job_start + args.budget_minutes * 60
    remaining = deadline - time.time()
    log(f"build budget left in this job: {remaining / 60:.0f} min")
    jobs = str(args.jobs or (os.cpu_count() or 4) + 2)
    ninja = ninja_exe()
    if remaining < 10 * 60:
        log("not enough time left, skipping ninja in this job")
        set_output("finished", "false")
        return
    cmd = [str(ninja), "-C", OUT_REL, "-j", jobs, "-k", "0", *BUILD_TARGETS]
    print("+", " ".join(cmd), flush=True)
    proc = subprocess.Popen(cmd, cwd=SRC, env=env_with_depot(),
                            creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
    try:
        rc = proc.wait(timeout=remaining)
        if rc != 0:
            # mz targets may lack deps on generated headers (race): retry once
            remaining = deadline - time.time()
            log(f"ninja rc={rc}; retrying once (missing-generated-header races), {remaining/60:.0f} min left")
            proc = subprocess.Popen(cmd, cwd=SRC, env=env_with_depot(),
                                    creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
            rc = proc.wait(timeout=max(remaining, 60))
    except subprocess.TimeoutExpired:
        log("time budget reached, interrupting ninja (CTRL_BREAK) so it can save state")
        for _ in range(3):
            try:
                os.kill(proc.pid, signal.CTRL_BREAK_EVENT)
            except OSError:
                pass
            time.sleep(2)
        try:
            proc.wait(timeout=180)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        set_output("finished", "false")
        remaining_steps(ninja)
        return
    if rc != 0:
        raise SystemExit(f"ninja failed with exit code {rc}")
    log("ninja finished all targets")
    set_output("finished", "true")


def remaining_steps(ninja: Path) -> None:
    try:
        out = subprocess.run([str(ninja), "-C", OUT_REL, "-n", *BUILD_TARGETS], cwd=SRC,
                             env=env_with_depot(), capture_output=True, text=True, timeout=900)
        lines = [l for l in out.stdout.splitlines() if l.startswith("[")]
        if lines:
            log("remaining: " + lines[-1].split("]")[0] + "]")
    except Exception as exc:  # noqa: BLE001
        print(f"could not count remaining steps: {exc}")


# ---------------------------------------------------------------- package
def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def cmd_package(_args) -> None:
    archive = OUT / "chrome.7z"
    if not archive.exists():
        raise SystemExit("out/Default/chrome.7z missing; mini_installer did not run")
    dist = WORK / "dist"
    stage = dist / "stage"
    shutil.rmtree(dist, ignore_errors=True)
    stage.mkdir(parents=True)
    run(["7z", "x", "-y", f"-o{dist / 'x'}", archive], env=os.environ)
    chrome_bin = dist / "x" / "Chrome-bin"
    shutil.copytree(chrome_bin, stage, dirs_exist_ok=True)
    version_dir = stage / TAG
    if not version_dir.is_dir():
        raise SystemExit(f"version dir {TAG} missing in chrome.7z")
    # Upstream package also carries the MSVC C++ runtime next to chrome.dll.
    for name in ("msvcp140.dll", "msvcp140_atomic_wait.dll"):
        src = OUT / name
        if src.exists() and not (version_dir / name).exists():
            shutil.copy2(src, version_dir / name)

    zip_name = f"mz-browser-kernel-{TAG}-win-x64.zip"
    zip_path = dist / zip_name
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for p in sorted(stage.rglob("*")):
            if p.is_file():
                z.write(p, p.relative_to(stage).as_posix())

    ours = sorted(p.relative_to(stage).as_posix() for p in stage.rglob("*") if p.is_file())
    ref_file = KERNEL / "ci" / "win" / f"reference-{TAG}-mz3.txt"
    lines = [f"## {zip_name}", "",
             f"- zip SHA-256: `{sha256(zip_path)}`",
             f"- chrome.dll SHA-256: `{sha256(version_dir / 'chrome.dll')}`",
             f"- files: {len(ours)}", ""]
    if ref_file.exists():
        ref = [l.strip() for l in ref_file.read_text(encoding="utf-8").splitlines()
               if l.strip() and not l.startswith("#")]
        missing = sorted(set(ref) - set(ours))
        extra = sorted(set(ours) - set(ref))
        lines.append(f"Compared with upstream file list ({len(ref)} files): "
                     f"{len(missing)} missing, {len(extra)} extra")
        lines += [f"- missing `{m}`" for m in missing[:50]]
        lines += [f"- extra `{e}`" for e in extra[:50]]
    summary("\n".join(lines))
    (dist / "SHA256SUMS.txt").write_text(f"{sha256(zip_path)}  {zip_name}\n", encoding="utf-8")


def main() -> None:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("sync")
    sub.add_parser("prepare")
    b = sub.add_parser("build")
    b.add_argument("--budget-minutes", type=float, required=True,
                   help="minutes after MZ_JOB_START at which ninja is interrupted")
    b.add_argument("--jobs", type=int, default=0)
    sub.add_parser("package")
    a = ap.parse_args()
    {"sync": cmd_sync, "prepare": cmd_prepare, "build": cmd_build, "package": cmd_package}[a.cmd](a)


if __name__ == "__main__":
    main()
