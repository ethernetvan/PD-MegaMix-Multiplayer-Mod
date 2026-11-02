import base64
import struct
import unittest
import zlib

from avatars import (AvatarStore, UploadGate, rgba_to_png, parse_upload,
                     OUT, _MAX_RGBA_STR)


def _chunks(png):
    pos = 8
    while pos < len(png):
        ln, tag = struct.unpack(">I4s", png[pos:pos + 8])
        yield tag, png[pos + 8:pos + 8 + ln]
        pos += 8 + ln + 4


def _img(w, h):
    """A non-flat w*h*4 RGBA test image (opaque, varied pixels)."""
    buf = bytearray()
    for i in range(w * h):
        buf += bytes(((i * 37) % 256, (i * 91) % 256,
                      (i * 53) % 256, 255))
    return bytes(buf)


def _msg(module=12, w=32, h=32, rgba=None):
    if rgba is None:
        rgba = base64.b64encode(_img(w, h)).decode()
    elif not isinstance(rgba, str):
        rgba = base64.b64encode(rgba).decode()
    return {"module": module, "w": w, "h": h, "rgba": rgba}


class PngTests(unittest.TestCase):
    def test_rgba_to_png_roundtrip(self):
        w, h = 4, 3
        rgba = bytes(range(w * h * 4))
        png = rgba_to_png(w, h, rgba)
        self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")
        chunks = dict(_chunks(png))
        iw, ih, depth, ctype = struct.unpack(">IIBB", chunks[b"IHDR"][:10])
        self.assertEqual((iw, ih, depth, ctype), (w, h, 8, 6))
        raw = zlib.decompress(chunks[b"IDAT"])
        rows = [raw[y * (w * 4 + 1):(y + 1) * (w * 4 + 1)]
                for y in range(h)]
        for y, row in enumerate(rows):
            self.assertEqual(row[0], 0)
            self.assertEqual(row[1:], rgba[y * w * 4:(y + 1) * w * 4])


class ParseTests(unittest.TestCase):
    def test_ok(self):
        module, w, h, png = parse_upload(_msg())
        self.assertEqual((module, w, h), (12, OUT, OUT))
        self.assertTrue(png.startswith(b"\x89PNG"))
        self.assertEqual([t for t, _ in _chunks(png)],
                         [b"IHDR", b"IDAT", b"IEND"])

    def test_rejects(self):
        for kw in ({"module": True}, {"module": -1},
                   {"module": 1000}, {"module": "12"},
                   {"w": 31}, {"w": 97}, {"h": 31},
                   {"rgba": "!!!"}):
            with self.assertRaises(ValueError, msg=str(kw)):
                parse_upload(_msg(**kw))
        # non-str rgba
        with self.assertRaises(ValueError):
            parse_upload({"module": 1, "w": 32, "h": 32, "rgba": b"x"})
        # oversize string rejected BEFORE decode
        with self.assertRaises(ValueError):
            parse_upload({"module": 1, "w": 96, "h": 96,
                          "rgba": "a" * (_MAX_RGBA_STR + 1)})
        # non-square
        with self.assertRaises(ValueError):
            parse_upload(_msg(w=32, h=64))
        # all-transparent
        with self.assertRaises(ValueError):
            parse_upload(_msg(rgba=bytes(32 * 32 * 4)))
        # flat (every pixel identical)
        with self.assertRaises(ValueError):
            parse_upload(_msg(rgba=b"\x11\x22\x33\x44" * (32 * 32)))

    def test_normalize_48_to_96(self):
        rgba = _img(48, 48)
        _, w, h, png = parse_upload(_msg(w=48, h=48, rgba=rgba))
        self.assertEqual((w, h), (OUT, OUT))
        chunks = dict(_chunks(png))
        iw, ih = struct.unpack(">II", chunks[b"IHDR"][:8])
        self.assertEqual((iw, ih), (96, 96))
        # a 48x48 pixel maps to a 2x2 block at 96x96 (nearest neighbour)
        raw = zlib.decompress(chunks[b"IDAT"])
        rows = [raw[y * (96 * 4 + 1) + 1:y * (96 * 4 + 1) + 1 + 96 * 4]
                for y in range(96)]
        self.assertEqual(rows[0][:4], rgba[:4])
        self.assertEqual(rows[1][:4], rgba[:4])   # pixel 0,0 replicated

    def test_transparent_rgb_zeroed(self):
        rgba = bytearray(_img(32, 32))
        rgba[0:3] = b"\xaa\xbb\xcc"
        rgba[3] = 0          # fully transparent with non-zero RGB
        _, _, _, png = parse_upload(_msg(rgba=bytes(rgba)))
        raw = zlib.decompress(dict(_chunks(png))[b"IDAT"])
        self.assertEqual(raw[1:4], b"\x00\x00\x00")   # rgb zeroed


class GateTests(unittest.TestCase):
    def test_mismatch_rate_limit_forget(self):
        g = UploadGate()
        self.assertEqual(g.check("a", 5, 6, 0.0)[1], "module_mismatch")
        for i in range(3):
            ok, _ = g.check("a", 5, 5, 1.0 + i)
            self.assertTrue(ok)
        self.assertEqual(g.check("a", 5, 5, 5.0)[1], "rate_limited")
        self.assertTrue(g.check("a", 5, 5, 70.0)[0])   # window passed
        g.forget("a")
        self.assertTrue(g.check("a", 5, 5, 71.0)[0])


class StoreTests(unittest.TestCase):
    def test_first_wins(self):
        s = AvatarStore()
        self.assertTrue(s.put(12, b"one"))
        self.assertFalse(s.put(12, b"two"))
        self.assertEqual(s.get(12), b"one")
        self.assertTrue(s.has(12))
        self.assertFalse(s.has(13))
        self.assertTrue(s.put(13, b"other"))


if __name__ == "__main__":
    unittest.main()
