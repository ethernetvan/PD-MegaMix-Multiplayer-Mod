import base64
import struct
import unittest
import zlib

from avatars import AvatarStore, rgba_to_png, parse_upload


def _png_chunks(png):
    pos = 8
    while pos < len(png):
        ln, tag = struct.unpack(">I4s", png[pos:pos + 8])
        yield tag, png[pos + 8:pos + 8 + ln]
        pos += 8 + ln + 4


class PngTests(unittest.TestCase):
    def test_rgba_to_png_roundtrip(self):
        w, h = 4, 3
        rgba = bytes(range(w * h * 4))
        png = rgba_to_png(w, h, rgba)
        self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")
        chunks = dict(_png_chunks(png))
        iw, ih, depth, ctype = struct.unpack(">IIBB", chunks[b"IHDR"][:10])
        self.assertEqual((iw, ih, depth, ctype), (w, h, 8, 6))
        raw = zlib.decompress(chunks[b"IDAT"])
        rows = [raw[y * (w * 4 + 1):(y + 1) * (w * 4 + 1)]
                for y in range(h)]
        for y, row in enumerate(rows):
            self.assertEqual(row[0], 0)                       # filter 0
            self.assertEqual(row[1:], rgba[y * w * 4:(y + 1) * w * 4])


class ParseTests(unittest.TestCase):
    def _msg(self, **kw):
        m = {"module": 12, "w": 4, "h": 4,
             "rgba": base64.b64encode(bytes(4 * 4 * 4)).decode()}
        m.update(kw)
        return m

    def test_ok(self):
        module, w, h, png = parse_upload(self._msg())
        self.assertEqual((module, w, h), (12, 4, 4))
        self.assertTrue(png.startswith(b"\x89PNG"))

    def test_rejects(self):
        for kw in ({"module": True}, {"module": -1},
                   {"module": 1000}, {"module": "12"},
                   {"w": 0}, {"w": 257}, {"w": True},
                   {"h": 0}, {"rgba": "!!!"},
                   {"rgba": base64.b64encode(b"x").decode()}):
            with self.assertRaises(ValueError, msg=str(kw)):
                parse_upload(self._msg(**kw))


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
