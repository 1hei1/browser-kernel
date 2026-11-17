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
TAG = os.environ.get("CHROMIUM_TAG", "158.0.8127.37")
BUILD_TARGETS = os.environ.get(
    "MZ_TARGETS",
    # use_static_angle=true (the 157/158 default) links ANGLE into chrome.dll and
    # mini_installer then skips libEGL/libGLESv2; upstream still ships the
    # dummy stub DLLs, so build them explicitly.
    "mini_installer ui/gl:generate_angle_stubs").split()


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
        # Keep the tree: the workflow packs it so a fixed commit can resume
        # (resume_run_id + ci/win/fixups) instead of rebuilding from scratch.
        log(f"ninja failed with exit code {rc}; packing the tree for a resume")
        set_output("finished", "failed")
        return
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


# ---------------------------------------------------------------- fixups
def cmd_fixups(_args) -> None:
    """Apply ci/win/fixups to a restored tree (resume after a failed stage).

    fixups/files/<path>   copied over chromium/src/<path>
    fixups/*.patch        applied once each (recorded in src/.mz-fixups-applied)
    fixups/*.py           run once each with chromium/src as argv[1]
    """
    root = KERNEL / "ci" / "win" / "fixups"
    if not root.is_dir():
        log("no fixups")
        return
    files = root / "files"
    if files.is_dir():
        for f in sorted(files.rglob("*")):
            if f.is_file():
                rel = f.relative_to(files)
                dst = SRC / rel
                dst.parent.mkdir(parents=True, exist_ok=True)
                if not dst.exists() or dst.read_bytes() != f.read_bytes():
                    shutil.copyfile(f, dst)
                    log(f"fixup file: {rel.as_posix()}")
    marker = SRC / ".mz-fixups-applied"
    done = set(marker.read_text(encoding="utf-8").split()) if marker.exists() else set()
    for patch in sorted(root.glob("*.patch")):
        if patch.name in done:
            continue
        try:
            run(["git", "apply", "--unsafe-paths", "--whitespace=nowarn", "-C1", patch], cwd=SRC)
        except subprocess.CalledProcessError:
            # Windows 上恢复出来的树可能是 CRLF，忽略空白再试一次
            run(["git", "apply", "--unsafe-paths", "--ignore-whitespace", "-C1", patch], cwd=SRC)
        done.add(patch.name)
        log(f"fixup patch: {patch.name}")
    # fixups/*.py：脚本自行修改 chromium/src（参数为 src 路径），只跑一次
    for script in sorted(root.glob("*.py")):
        if script.name in done:
            continue
        run([sys.executable, script, SRC])
        done.add(script.name)
        log(f"fixup script: {script.name}")
    marker.write_text("\n".join(sorted(done)) + "\n", encoding="utf-8")


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

    # Upstream's zip is assembled from the build dir, not only chrome.7z:
    #  - chrome.release still names chrome.exe, so the renamed exe is skipped
    #  - static ANGLE makes mini_installer skip the libEGL/libGLESv2 stubs
    #  - the developer-build VisualElements manifest sits in VisualElements/
    #  - no Extensions/external_extensions.json
    exe = os.environ.get("MZ_EXE_NAME", "mz") + ".exe"
    if not (OUT / exe).exists():
        raise SystemExit(f"{exe} missing in build dir")
    shutil.copy2(OUT / exe, stage / exe)
    if exe != "chrome.exe":
        (stage / "chrome.exe").unlink(missing_ok=True)
    for name in ("libEGL.dll", "libGLESv2.dll"):
        if not (OUT / name).exists():
            raise SystemExit(f"{name} missing in build dir (ui/gl:generate_angle_stubs not built?)")
        shutil.copy2(OUT / name, version_dir / name)
    ve = version_dir / "VisualElements"
    ve.mkdir(exist_ok=True)
    shutil.copy2(OUT / "chrome.VisualElementsManifest.xml", ve / "chrome.VisualElementsManifest.xml")
    (version_dir / "Extensions" / "external_extensions.json").unlink(missing_ok=True)
    try:
        (version_dir / "Extensions").rmdir()
    except OSError:
        pass

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
    sub.add_parser("fixups")
    a = ap.parse_args()
    {"sync": cmd_sync, "prepare": cmd_prepare, "build": cmd_build, "package": cmd_package,
     "fixups": cmd_fixups}[a.cmd](a)


if __name__ == "__main__":
    main()
