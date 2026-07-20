#!/usr/bin/env python3
"""Host-side smoke test for the 64-color median-cut approach used by
pd-content's auto_quantize_palette path. Not a firmware emulator — just
checks that sampling + median-cut produces a palette of <=64 colors and
that nearest-index remapping round-trips opaque pixels with low error.
"""

from __future__ import annotations

import random
import struct
import zlib
from pathlib import Path

PD_PALETTE_MAX = 64


def nearest(palette, r, g, b, a):
    best_i, best_d = 0, 1 << 30
    for i, (pr, pg, pb, pa) in enumerate(palette):
        d = (r - pr) ** 2 + (g - pg) ** 2 + (b - pb) ** 2 + (a - pa) ** 2
        if d < best_d:
            best_d, best_i = d, i
            if d == 0:
                break
    return best_i


def median_cut(samples: list[tuple[int, int, int, int]], max_colors: int = PD_PALETTE_MAX):
    if not samples:
        return [(0, 0, 0, 0)]
    # dedupe if already small
    uniq = list(dict.fromkeys(samples))
    if len(uniq) <= max_colors:
        return uniq

    boxes = [list(samples)]
    while len(boxes) < max_colors:
        best = None
        best_range = -1
        best_ch = 0
        for bi, box in enumerate(boxes):
            if len(box) < 2:
                continue
            for ch in range(4):
                vals = [c[ch] for c in box]
                rng = max(vals) - min(vals)
                if rng > best_range:
                    best_range = rng
                    best = bi
                    best_ch = ch
        if best is None or best_range <= 0:
            break
        box = sorted(boxes[best], key=lambda c: c[best_ch])
        mid = max(1, min(len(box) - 1, len(box) // 2))
        boxes[best] = box[:mid]
        boxes.append(box[mid:])

    palette = []
    for box in boxes:
        n = len(box)
        palette.append(tuple(sum(c[i] for c in box) // n for i in range(4)))
    return palette


def load_png_rgba(path: Path):
    data = path.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    # Minimal PNG decoder for 8-bit RGBA/RGB/palette via pillow if available,
    # else skip. Prefer pillow.
    try:
        from PIL import Image
    except ImportError:
        return None
    im = Image.open(path).convert("RGBA")
    return im.width, im.height, list(im.getdata())


def test_median_cut_size():
    random.seed(1)
    samples = [
        (random.randint(0, 255), random.randint(0, 255), random.randint(0, 255), 255)
        for _ in range(4000)
    ]
    pal = median_cut(samples, 64)
    assert 1 <= len(pal) <= 64, len(pal)
    print(f"median_cut: {len(pal)} colors from {len(samples)} samples — OK")


def test_content_sequence_if_present():
    root = Path(__file__).resolve().parents[1] / "content" / "images"
    seq = root / "pac-ghost"
    if not seq.is_dir():
        print("skip: no pac-ghost content")
        return
    frames = sorted(seq.glob("*.png"))[:8]
    if not frames:
        print("skip: no png frames")
        return
    samples = []
    for fp in frames:
        loaded = load_png_rgba(fp)
        if not loaded:
            print("skip: Pillow not installed")
            return
        w, h, px = loaded
        step = max(1, (w * h) // 1024)
        for i in range(0, len(px), step):
            r, g, b, a = px[i]
            if a:
                samples.append((r, g, b, a))
    pal = median_cut(samples, 64)
    assert len(pal) <= 64
    # Remap one frame and check mean absolute error is bounded
    w, h, px = load_png_rgba(frames[0])
    err = 0
    n = 0
    for r, g, b, a in px:
        if a == 0:
            continue
        i = nearest(pal, r, g, b, a)
        pr, pg, pb, pa = pal[i]
        err += abs(r - pr) + abs(g - pg) + abs(b - pb)
        n += 1
    mae = err / max(1, n * 3)
    print(f"pac-ghost sample: palette={len(pal)} MAE={mae:.2f} — OK")
    assert mae < 40.0, mae


if __name__ == "__main__":
    test_median_cut_size()
    test_content_sequence_if_present()
    print("all palette quantize smoke tests passed")
