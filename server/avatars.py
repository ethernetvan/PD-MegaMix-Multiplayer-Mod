"""Avatar portrait store: consoles upload their module's 96x96 RGBA
portrait as base64 in an `avatar_img` message; the server keeps the
first upload per module for its lifetime and serves it as PNG to the
web UI. Ports are game assets - never committed to the repo."""

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


def parse_upload(msg) -> tuple[int, int, int, bytes]:
    """avatar_img {module:int 0..999, w:int 1..256, h:int 1..256,
    rgba:base64 of exactly w*h*4 bytes} -> (module, w, h, png)."""
    module = msg.get("module")
    if isinstance(module, bool) or not isinstance(module, int) \
            or not (0 <= module <= 999):
        raise ValueError("module must be an int in 0..999")
    w, h = msg.get("w"), msg.get("h")
    for name, v in (("w", w), ("h", h)):
        if isinstance(v, bool) or not isinstance(v, int) \
                or not (1 <= v <= 256):
            raise ValueError(f"{name} must be an int in 1..256")
    try:
        rgba = base64.b64decode(msg.get("rgba", ""), validate=True)
    except Exception:
        raise ValueError("rgba is not valid base64")
    if len(rgba) != w * h * 4:
        raise ValueError(f"rgba decodes to {len(rgba)} bytes, "
                         f"expected {w * h * 4}")
    return module, w, h, rgba_to_png(w, h, rgba)


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
