"""Avatar portrait store: consoles upload their module's 96x96 RGBA
portrait as base64 in an `avatar_img` message; the server keeps the
first upload per module for its lifetime and serves it as PNG to the
web UI. Portraits are memory-only, never written to disk, and are only
ever named by the integer module id (`/api/avatar/<module>.png`,
`avatar_NNN.png` in Content-Disposition) - uploader-supplied names or
metadata are never used. Portraits are game assets - never committed
to the repo."""

import base64
import struct
import zlib


def rgba_to_png(w: int, h: int, rgba: bytes) -> bytes:
    """Pack raw RGBA rows (top-first, no filtering) into a PNG."""
    raw = b"".join(b"\x00" + rgba[y * w * 4:(y + 1) * w * 4]
                   for y in range(h))

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 6))
            + chunk(b"IEND", b""))


OUT = 96
# base64 of a 96x96x4 payload, checked BEFORE decoding
_MAX_RGBA_STR = 4 * ((96 * 96 * 4 + 2) // 3)


def parse_upload(msg) -> tuple[int, int, int, bytes]:
    """avatar_img {module:int 0..999, w:int, h:int (square, 32..96),
    rgba:base64 of exactly w*h*4 bytes} -> (module, 96, 96, png).

    Normalizes to 96x96 (nearest-neighbour upscale), zeros the RGB of
    fully transparent pixels, and re-encodes with rgba_to_png, so the
    returned PNG only ever carries IHDR/IDAT/IEND and nothing from the
    uploader survives except pixel values."""
    module = msg.get("module")
    if isinstance(module, bool) or not isinstance(module, int) \
            or not (0 <= module <= 999):
        raise ValueError("module must be an int in 0..999")
    w, h = msg.get("w"), msg.get("h")
    for name, v in (("w", w), ("h", h)):
        if isinstance(v, bool) or not isinstance(v, int) \
                or not (32 <= v <= 96):
            raise ValueError(
                f"{name} must be an int in 32..96 (got {v})")
    if w != h:
        raise ValueError(f"image must be square (got {w}x{h})")
    b64 = msg.get("rgba")
    if not isinstance(b64, str):
        raise ValueError("rgba must be a base64 string")
    if len(b64) > _MAX_RGBA_STR:
        raise ValueError(
            f"rgba string too long ({len(b64)} > {_MAX_RGBA_STR})")
    try:
        rgba = base64.b64decode(b64, validate=True)
    except Exception:
        raise ValueError("rgba is not valid base64")
    if len(rgba) != w * h * 4:
        raise ValueError(f"{w}x{h} expects {w * h * 4} bytes, "
                         f"decoded {len(rgba)}")
    # garbage guards: all-transparent or a flat single colour
    if max(rgba[3::4]) == 0:
        raise ValueError(f"{w}x{h} image is fully transparent")
    if rgba == rgba[:4] * (w * h):
        raise ValueError(f"{w}x{h} image is flat (one colour)")
    # normalize to 96x96: nearest-neighbour upscale + zero the RGB of
    # fully transparent pixels (no hidden data in invisible pixels)
    if w != OUT:
        up = bytearray(OUT * OUT * 4)
        for y in range(OUT):
            sy = y * w // OUT
            for x in range(OUT):
                sx = x * w // OUT
                up[(y * OUT + x) * 4:(y * OUT + x) * 4 + 4] = \
                    rgba[(sy * w + sx) * 4:(sy * w + sx) * 4 + 4]
        rgba = bytes(up)
    out = bytearray(rgba)
    for i in range(3, len(out), 4):
        if out[i] == 0:
            out[i - 3] = out[i - 2] = out[i - 1] = 0
    return module, OUT, OUT, rgba_to_png(OUT, OUT, bytes(out))


class UploadGate:
    """Per-cid upload admission: the module must match the client's
    announced `avatar {module}` and at most 3 avatar_img attempts are
    allowed per 60 s (attempts count, not just accepted uploads)."""

    RATE_N = 3
    RATE_S = 60.0

    def __init__(self):
        self._attempts: dict = {}   # cid -> [timestamps]

    def check(self, cid, module, announced_module, now):
        if module != announced_module:
            return False, "module_mismatch"
        ts = [t for t in self._attempts.get(cid, [])
              if now - t < self.RATE_S]
        ts.append(now)
        self._attempts[cid] = ts
        if len(ts) > self.RATE_N:
            return False, "rate_limited"
        return True, ""

    def forget(self, cid):
        self._attempts.pop(cid, None)


class AvatarStore:
    """First upload per module wins for the server's lifetime."""

    def __init__(self):
        self._png: dict[int, bytes] = {}

    def put(self, module: int, png: bytes) -> bool:
        if module in self._png:
            return False
        self._png[module] = png
        return True

    def get(self, module: int):
        return self._png.get(module)

    def has(self, module: int) -> bool:
        return module in self._png
