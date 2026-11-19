"""
passkey deploy: 把 sources/ 下的文件按原路径复制到 Chromium 源码。
"""
import shutil
from pathlib import Path


def run(chromium_src: Path, kernel_root: Path, project_root: Path) -> None:
    src_root = project_root / "sources"
    for f in sorted(src_root.rglob("*")):
        if f.is_file():
            rel = f.relative_to(src_root)
            dst = chromium_src / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, dst)
            print(f"  {rel.as_posix()}")
