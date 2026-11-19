"""续编修复：品牌目录 chrome/app/theme/mz 缺少 linux/ 等子目录（deploy_resources 早期只处理 win）。
从 theme/chromium 补齐缺失文件，linux/product_logo_<N>.png 优先用 overlay/branding/icons/all/<N>x<N>.png。"""
import re, shutil, sys
from pathlib import Path

src = Path(sys.argv[1])
icons = Path(__file__).resolve().parents[3] / "overlay" / "branding" / "icons" / "all"
theme = src / "chrome" / "app" / "theme"
chromium, dst = theme / "chromium", theme / "mz"
for f in chromium.rglob("*"):
    if not f.is_file():
        continue
    d = dst / f.relative_to(chromium)
    if d.exists():
        continue
    d.parent.mkdir(parents=True, exist_ok=True)
    s = f
    m = re.fullmatch(r"product_logo_(\d+)\.png", f.name)
    if m and f.parent.name == "linux":
        c = icons / f"{m.group(1)}x{m.group(1)}.png"
        if c.is_file():
            s = c
    shutil.copy2(s, d)
    print("fixup theme/mz/%s" % d.relative_to(dst))
