"""Avatar portrait store: consoles upload their module's 96x96 RGBA
portrait as base64 in an `avatar_img` message. The server keeps the
sanitized pixels per uploader, serves them as PNG to the web UI and, on
request, relays them to opted-in consoles in the same lobby (custom/mod
modules the other console doesn't have). Portraits are memory-only,
never written to disk, and are only ever named by their content id (the
first 12 hex digits of the SHA-256 of the normalized RGBA:
`/api/avatar/<id>.png`, `avatar_<id>.png` in Content-Disposition) -
uploader-supplied names or metadata are never used. Portraits are game
assets - never committed to the repo."""

import base64
import hashlib
import re
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


def parse_upload(msg) -> tuple[int, int, int, bytes, bytes]:
    """avatar_img {module:int 0..999, w:int, h:int (square, 32..96),
    rgba:base64 of exactly w*h*4 bytes} -> (module, w, h, rgba96, png):
    rgba96 = the normalized 96x96 pixels consoles get, png = its PNG.

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
    return module, w, h, bytes(out), rgba_to_png(OUT, OUT, bytes(out))


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


IMG_ID = re.compile(r"[0-9a-f]{12}")


class RelayGate:
    """Per-cid avatar_get admission: at most 20 requests per 60 s."""

    RATE_N = 20
    RATE_S = 60.0

    def __init__(self):
        self._t: dict = {}

    def allow(self, cid, now):
        ts = [t for t in self._t.get(cid, []) if now - t < self.RATE_S]
        ts.append(now)
        self._t[cid] = ts
        return len(ts) <= self.RATE_N

    def forget(self, cid):
        self._t.pop(cid, None)


class AvatarStore:
    """Sanitized portraits keyed by content id, memory only, at most MAX
    (oldest dropped). A hidden (moderated) id is never served again."""

    MAX = 256

    def __init__(self):
        self._img: dict[str, tuple[bytes, bytes]] = {}   # id -> (rgba, png)
        self._hidden: set[str] = set()

    def put(self, rgba: bytes, png: bytes) -> str:
        img = hashlib.sha256(rgba).hexdigest()[:12]
        if img in self._img:
            self._img[img] = self._img.pop(img)      # most recent last
        else:
            self._img[img] = (rgba, png)
            while len(self._img) > self.MAX:
                self._img.pop(next(iter(self._img)))
        return img

    def has(self, img) -> bool:
        return img in self._img and img not in self._hidden

    def png(self, img):
        return self._img[img][1] if self.has(img) else None

    def rgba(self, img):
        return self._img[img][0] if self.has(img) else None

    def hide(self, img):
        self._hidden.add(img)

    def hidden(self, img) -> bool:
        return img in self._hidden
