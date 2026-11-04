import base64
import struct
import unittest
import zlib

from avatars import (AvatarStore, UploadGate, RelayGate, rgba_to_png,
                     parse_upload, OUT, _MAX_RGBA_STR)
from mp_server import Server


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
        module, w, h, rgba, png = parse_upload(_msg())
        self.assertEqual((module, w, h, len(rgba)), (12, 32, 32, OUT * OUT * 4))
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
        _, w, h, rgba96, png = parse_upload(_msg(w=48, h=48, rgba=rgba))
        self.assertEqual((w, h), (48, 48))
        self.assertEqual(rgba96[96 * 4:96 * 4 + 4], rgba[:4])
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
        _, _, _, rgba96, png = parse_upload(_msg(rgba=bytes(rgba)))
        self.assertEqual(rgba96[:4], b"\x00\x00\x00\x00")
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
    def test_content_ids_hide_and_cap(self):
        s = AvatarStore()
        a = s.put(b"pixels-a", b"png-a")
        self.assertRegex(a, r"^[0-9a-f]{12}$")
        self.assertEqual(s.put(b"pixels-a", b"png-a"), a)   # same content
        b = s.put(b"pixels-b", b"png-b")
        self.assertNotEqual(a, b)
        self.assertEqual((s.png(a), s.rgba(a)), (b"png-a", b"pixels-a"))
        s.hide(a)
        self.assertFalse(s.has(a))
        self.assertIsNone(s.png(a))
        self.assertIsNone(s.rgba(a))
        self.assertTrue(s.hidden(a))
        s.MAX = 3
        ids = [s.put(bytes([i]), b"p") for i in range(5)]
        self.assertEqual(len(s._img), 3)
        self.assertFalse(s.has(ids[0]))
        self.assertTrue(s.has(ids[4]))

    def test_relay_gate(self):
        g = RelayGate()
        self.assertTrue(all(g.allow("a", 1.0) for _ in range(20)))
        self.assertFalse(g.allow("a", 2.0))
        self.assertTrue(g.allow("b", 2.0))
        self.assertTrue(g.allow("a", 70.0))


def _upload(w=32):
    return {"t": "avatar_img", "module": 437, "w": w, "h": w,
            "rgba": base64.b64encode(_img(w, w)).decode()}


class ServerRelayTests(unittest.TestCase):
    """avatar_img / avatar_get / hide on the real Server (no sockets)."""

    def setUp(self):
        self.s = Server(0, 0)
        lm = self.lm = self.s.lobby
        for cid, name in (("A", "Ann"), ("B", "Ben"), ("C", "Cy")):
            lm.on_hello(cid, name, "v1", 1.0)
        lm.on_message("A", {"t": "lobby_create"}, 1.0)
        code = next(iter(lm.lobbies))
        lm.on_message("B", {"t": "lobby_join", "code": code}, 1.0)
        lm.on_message("A", {"t": "avatar", "module": 437, "chara": 5,
                            "mh": 0x54ca14ea}, 1.0)
        lm.outbox.clear()

    def test_upload_member_fields_and_relay(self):
        ok, info = self.s.accept_upload("A", _upload(), 2.0)
        self.assertTrue(ok, info)
        img = self.s.client_img["A"][1]
        lob = [m for cid, m in self.lm.outbox
               if cid == "B" and m["t"] == "lobby"][-1]
        ann = [m for m in lob["members"] if m["cid"] == "A"][0]
        self.assertEqual((ann["module"], ann["chara"], ann["mh"], ann["img"]),
                         (437, 5, 0x54ca14ea, img))
        # the same module again is skipped (no decode, no rate use)
        self.assertEqual(self.s.accept_upload("A", _upload(), 3.0),
                         (False, ""))
        r = self.s.avatar_get_reply("B", img, 4.0)
        self.assertEqual(len(base64.b64decode(r["rgba"])), OUT * OUT * 4)
        # C is not in the lobby; junk ids are refused
        self.assertEqual(self.s.avatar_get_reply("C", img, 4.0)["err"],
                         "not_in_lobby")
        self.assertEqual(self.s.avatar_get_reply("B", "../x", 4.0)["err"],
                         "bad_img")
        self.assertEqual(self.s.avatar_get_reply("B", "0" * 12, 4.0)["err"],
                         "not_in_lobby")
        # relay off: consoles see no img and can't fetch; web still can
        self.s.avatar_relay = False
        self.assertEqual(self.s.console_img("A"), "")
        self.assertEqual(self.s.web_img("A"), img)
        self.assertEqual(self.s.avatar_get_reply("B", img, 5.0)["err"],
                         "relay_off")
        self.s.avatar_relay = True
        # hidden by a moderator: gone for consoles and the web
        self.assertTrue(self.s.hide_avatar("A")["ok"])
        self.assertEqual(self.s.avatar_get_reply("B", img, 6.0)["err"],
                         "hidden")
        self.assertEqual(self.s.web_img("A"), "")

    def test_upload_gated(self):
        bad = dict(_upload(), module=12)       # not the announced module
        self.assertEqual(self.s.accept_upload("A", bad, 2.0),
                         (False, "module_mismatch"))
        flat = dict(_upload(), rgba=base64.b64encode(
            b"\x01\x02\x03\xff" * 32 * 32).decode())
        ok, reason = self.s.accept_upload("A", flat, 2.0)
        self.assertFalse(ok)
        self.assertIn("flat", reason)

    def test_avatar_validation(self):
        self.lm.on_message("B", {"t": "avatar", "module": 5, "chara": "x",
                                 "mh": -4}, 2.0)
        self.assertEqual(self.lm._client_avatar["B"], (-1, 0))
        self.lm.on_message("B", {"t": "avatar", "module": 5}, 2.0)
        self.assertEqual(self.lm._client_modules["B"], 5)

    def test_get_rate_limited(self):
        self.s.accept_upload("A", _upload(), 2.0)
        img = self.s.client_img["A"][1]
        for _ in range(20):
            self.assertIn("rgba", self.s.avatar_get_reply("B", img, 3.0))
        self.assertEqual(self.s.avatar_get_reply("B", img, 3.0)["err"],
                         "rate_limited")


if __name__ == "__main__":
    unittest.main()
