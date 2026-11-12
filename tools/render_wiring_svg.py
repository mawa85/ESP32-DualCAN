#!/usr/bin/env python3
"""Render docs/img/wiring-t2can-fd-s3xy.svg to a PNG next to it.

Uses cairosvg when it is installed. The committed PNG is the output of
this script; re-run it after editing the SVG.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SVG = ROOT / "docs" / "img" / "wiring-t2can-fd-s3xy.svg"
PNG = ROOT / "docs" / "img" / "wiring-t2can-fd-s3xy.png"


def main() -> int:
    if not SVG.is_file():
        print(f"missing {SVG}", file=sys.stderr)
        return 1
    import cairosvg

    cairosvg.svg2png(url=str(SVG), write_to=str(PNG), output_width=1200, output_height=820)
    print(PNG)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
