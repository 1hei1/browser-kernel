"""
TOTP deploy: 把 sources/mz/totp 复制到 Chromium 源码 mz/totp。
"""
import shutil
from pathlib import Path


def run(chromium_src: Path, kernel_root: Path, project_root: Path) -> None:
    src = project_root / "sources" / "mz" / "totp"
    dst = chromium_src / "mz" / "totp"
    dst.mkdir(parents=True, exist_ok=True)
    for f in src.iterdir():
        if f.is_file():
            shutil.copy2(f, dst / f.name)
            print(f"  mz/totp/{f.name}")
