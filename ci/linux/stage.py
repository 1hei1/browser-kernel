#!/usr/bin/env python3
"""MZ kernel Linux x64 CI helper (headless servers / agents).

Subcommands (all run on a GitHub-hosted ubuntu runner):
  sync      fetch Chromium <tag> with depot_tools (shallow, no history) + runhooks
  prepare   write out/Default/args.gn, run the MZ driver (apply + deploy), gn gen
  build     run ninja until it finishes or the time budget for this job runs out
  fixups    apply ci/linux/fixups to a restored tree (resume after a failed stage)
  package   collect the runtime files of out/Default into mz-browser-kernel-<ver>-linux-x64.zip

Layout under $MZ_WORK (chosen by the workflow):
  depot_tools/        chromium/.gclient   chromium/src/   dist/
"""
from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import signal
import subprocess
import sys
import zipfile
import time
from pathlib import Path

KERNEL = Path(__file__).resolve().parents[2]
CI = Path(__file__).resolve().parent
WORK = Path(os.environ.get("MZ_WORK", "/mnt/mzb"))
DEPOT = WORK / "depot_tools"
CHROMIUM = WORK / "chromium"
SRC = CHROMIUM / "src"
OUT_REL = "out/Default"
OUT = SRC / OUT_REL
TAG = os.environ.get("CHROMIUM_TAG", "158.0.8127.37")
BUILD_TARGETS = os.environ.get("MZ_TARGETS", "chrome").split()


def log(msg: str) -> None:
    print(f"::notice::{msg}" if os.environ.get("GITHUB_ACTIONS") else msg, flush=True)


def env_with_depot() -> dict:
    env = dict(os.environ)
    env["PATH"] = str(DEPOT) + os.pathsep + env["PATH"]
    env["DEPOT_TOOLS_UPDATE"] = "0"
    env["DEPOT_TOOLS_METRICS"] = "0"
    return env


def run(cmd, cwd=None, env=None, check=True):
    print("+", " ".join(map(str, cmd)), flush=True)
    return subprocess.run([str(c) for c in cmd], cwd=cwd, env=env or env_with_depot(),
                          check=check)


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
    env["DEPOT_TOOLS_UPDATE"] = "1"
    run(["gclient", "--version"], env=env, check=False)

    CHROMIUM.mkdir(parents=True, exist_ok=True)
    (CHROMIUM / ".gclient").write_text(
        "solutions = [{\n"
        "  'name': 'src',\n"
        "  'url': 'https://chromium.googlesource.com/chromium/src.git',\n"
        "  'managed': False,\n"
        "  'custom_deps': {},\n"
        "  'custom_vars': {'checkout_pgo_profiles': False, 'checkout_nacl': False},\n"
        "}]\n"
        "target_os = ['linux']\n",
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
    run(["gclient", "sync", "--no-history", "--shallow", "--nohooks",
         "-D", "--force", "-j", "16"], cwd=CHROMIUM)
    log(f"gclient sync took {(time.time() - t0) / 60:.1f} min")
    # 构建依赖（sysroot/clang 由 runhooks 下载；这里只补宿主工具）
    run(["sudo", "env", "DEBIAN_FRONTEND=noninteractive", str(SRC / "build" / "install-build-deps.sh"),
         "--no-prompt", "--no-arm", "--no-chromeos-fonts"], check=False)
    t0 = time.time()
    run(["gclient", "runhooks"], cwd=CHROMIUM)
    log(f"gclient runhooks took {(time.time() - t0) / 60:.1f} min")
    version = (SRC / "chrome" / "VERSION").read_text().split()
    log("chrome/VERSION: " + " ".join(version))


# ---------------------------------------------------------------- prepare
def cmd_prepare(_args) -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(CI / "args.gn", OUT / "args.gn")

    env = env_with_depot()
    env["MZ_CHROMIUM_ROOT"] = str(SRC)
    env["MZ_OUT_DIR"] = OUT_REL
    run(["uv", "run", "python", "-m", "driver", "validate"], cwd=KERNEL, env=env)
    run(["uv", "run", "python", "-m", "driver", "--out-dir", OUT_REL,
         "apply-and-prepare"], cwd=KERNEL, env=env)
    # Linux 上可执行文件名保持 chrome（installer/linux 等目标写死了 chrome）
    args = (OUT / "args.gn").read_text(encoding="utf-8")
    args = re.sub(r'(?m)^\s*chrome_executable_name\s*=.*$', 'chrome_executable_name = "chrome"', args)
    (OUT / "args.gn").write_text(args, encoding="utf-8")
    print("---- final args.gn ----")
    print(args)
    run(["gn", "gen", OUT_REL], cwd=SRC)
    run(["gn", "args", OUT_REL, "--list", "--short", "--overrides-only"], cwd=SRC, check=False)
    fill_missing_brand_sources()


def fill_missing_brand_sources() -> None:
    """branding 把 branding_path_component/product 设成 mz，GN 会要一些 overlay 没带的
    品牌文件（mz_* 或 theme/mz/...）。ninja 试运行，把缺的文件从 chromium 对应文件复制过来。"""
    pat = re.compile(r"'([^']+)', needed by '[^']+', missing and no known rule")
    for _ in range(50):
        r = subprocess.run([str(ninja_exe()), "-C", OUT_REL, "-n", *BUILD_TARGETS],
                           cwd=SRC, capture_output=True, text=True, errors="replace")
        m = pat.search(r.stdout + r.stderr)
        if not m:
            if r.returncode != 0:
                print((r.stdout + r.stderr)[-4000:])
                raise SystemExit("ninja dry run failed")
            log("ninja dry run OK: no missing inputs")
            return
        missing = (OUT / m.group(1)).resolve()
        candidates = []
        if "mz_" in missing.name:
            candidates.append(missing.with_name(missing.name.replace("mz_", "chromium_", 1)))
        rel = missing.relative_to(SRC).as_posix()
        if "/mz/" in rel:
            candidates.append(SRC / rel.replace("/mz/", "/chromium/", 1))
        src = next((c for c in candidates if c.exists()), None)
        if src is None:
            raise SystemExit(f"missing input with no chromium counterpart: {missing}")
        missing.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, missing)
        log(f"created {rel} from {src.relative_to(SRC).as_posix()}")
    raise SystemExit("too many missing inputs")


# ---------------------------------------------------------------- build
def ninja_exe() -> Path:
    for p in (SRC / "third_party" / "ninja" / "ninja", DEPOT / "ninja"):
        if p.exists():
            return p
    raise SystemExit("ninja not found")


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

    def start():
        print("+", " ".join(cmd), flush=True)
        return subprocess.Popen(cmd, cwd=SRC, env=env_with_depot(), start_new_session=True)

    proc = start()
    try:
        rc = proc.wait(timeout=remaining)
        if rc != 0:
            remaining = deadline - time.time()
            log(f"ninja rc={rc}; retrying once (missing-generated-header races), {remaining/60:.0f} min left")
            proc = start()
            rc = proc.wait(timeout=max(remaining, 60))
    except subprocess.TimeoutExpired:
        log("time budget reached, interrupting ninja (SIGINT) so it can save state")
        try:
            os.killpg(proc.pid, signal.SIGINT)
        except OSError:
            pass
        try:
            proc.wait(timeout=180)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
            proc.wait()
        set_output("finished", "false")
        remaining_steps(ninja)
        return
    if rc != 0:
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
    """fixups/files/<path> 覆盖 chromium/src/<path>；fixups/*.patch 只打一次；fixups/*.py 只跑一次。"""
    root = CI / "fixups"
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
        run(["git", "apply", "--unsafe-paths", "--whitespace=nowarn", "-C1", patch], cwd=SRC)
        done.add(patch.name)
        log(f"fixup patch: {patch.name}")
    for script in sorted(root.glob("*.py")):
        if script.name in done:
            continue
        run([sys.executable, script, SRC])
        done.add(script.name)
        log(f"fixup script: {script.name}")
    marker.write_text("\n".join(sorted(done)) + "\n", encoding="utf-8")


# ---------------------------------------------------------------- package
RUNTIME_FILES = [
    "chrome", "chrome_crashpad_handler", "chrome-wrapper",
    "chrome_100_percent.pak", "chrome_200_percent.pak", "resources.pak",
    "icudtl.dat", "v8_context_snapshot.bin", "snapshot_blob.bin",
    "libEGL.so", "libGLESv2.so", "libvk_swiftshader.so", "vk_swiftshader_icd.json",
    "libvulkan.so.1", "libqt5_shim.so", "libqt6_shim.so",
    "xdg-mime", "xdg-settings", "product_logo_48.png",
]
RUNTIME_DIRS = ["locales", "MEIPreload", "PrivacySandboxAttestationsPreloaded", "resources"]


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def cmd_package(_args) -> None:
    """打成和 Windows 包同样结构的 zip：文件都在根目录，主程序叫 mz（应用按这个名字找），
    zip 里保留 unix 权限位（应用解压时还原可执行位）。"""
    if not (OUT / "chrome").exists():
        raise SystemExit("out/Default/chrome missing")
    dist = WORK / "dist"
    shutil.rmtree(dist, ignore_errors=True)
    stage = dist / "stage"
    stage.mkdir(parents=True)
    for name in RUNTIME_FILES:
        p = OUT / name
        if p.exists():
            shutil.copy2(p, stage / ("mz" if name == "chrome" else name))
    for name in RUNTIME_DIRS:
        p = OUT / name
        if p.is_dir():
            shutil.copytree(p, stage / name)
    for name in ("mz", "chrome_crashpad_handler"):
        if (stage / name).exists():
            subprocess.run(["strip", "--strip-unneeded", str(stage / name)], check=False)
    (stage / "VERSION").write_text(TAG + "\n", encoding="utf-8")

    zip_name = f"mz-browser-kernel-{TAG}-linux-x64.zip"
    zip_path = dist / zip_name
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for p in sorted(stage.rglob("*")):
            if p.is_file():
                info = zipfile.ZipInfo.from_file(p, p.relative_to(stage).as_posix())
                info.compress_type = zipfile.ZIP_DEFLATED
                with p.open("rb") as f:
                    z.writestr(info, f.read(), compresslevel=9)
    files = sorted(p.relative_to(stage).as_posix() for p in stage.rglob("*") if p.is_file())
    summary("\n".join([f"## {zip_name}", "",
                       f"- zip SHA-256: `{sha256(zip_path)}`",
                       f"- mz SHA-256 (signature): `{sha256(stage / 'mz')}`",
                       f"- size: {zip_path.stat().st_size} bytes",
                       f"- files: {len(files)}", ""] +
                      [f"- `{f}`" for f in files if "/" not in f]))
    (dist / "SHA256SUMS.txt").write_text(f"{sha256(zip_path)}  {zip_name}\n", encoding="utf-8")


def main() -> None:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("sync")
    sub.add_parser("prepare")
    b = sub.add_parser("build")
    b.add_argument("--budget-minutes", type=float, required=True)
    b.add_argument("--jobs", type=int, default=0)
    sub.add_parser("package")
    sub.add_parser("fixups")
    a = ap.parse_args()
    {"sync": cmd_sync, "prepare": cmd_prepare, "build": cmd_build, "package": cmd_package,
     "fixups": cmd_fixups}[a.cmd](a)


if __name__ == "__main__":
    main()
