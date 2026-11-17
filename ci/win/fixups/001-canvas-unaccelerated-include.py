"""fingerprint/045 在 158 上缺 unaccelerated_static_bitmap_image.h（不再被间接包含）。

给已恢复的编译树补上 include；幂等，保留原有换行风格。
"""
import sys
from pathlib import Path

src = Path(sys.argv[1])
f = src / "third_party/blink/renderer/core/html/canvas/html_canvas_element.cc"
data = f.read_bytes()
inc = b'#include "third_party/blink/renderer/platform/graphics/unaccelerated_static_bitmap_image.h"'
if inc in data:
    print("already present")
    sys.exit(0)
nl = b"\r\n" if b"\r\n" in data else b"\n"
anchor = nl + b"namespace blink {"
i = data.find(anchor)
if i < 0:
    sys.exit("anchor 'namespace blink {' not found")
data = data[:i] + nl + inc + data[i:]
f.write_bytes(data)
print("include added")
