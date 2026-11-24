import asyncio
import json
import socket
import unittest

import netcheck
from mp_server import DiscoveryProtocol


class FirewallProblemsTests(unittest.TestCase):
    def test_allow_any_covers_public(self):
        self.assertEqual(netcheck.firewall_problems(
            ["Public"], [{"action": "Allow", "profile": "Any"}]), [])

    def test_private_only_rule_misses_public(self):
        probs = netcheck.firewall_problems(
            ["Public"], [{"action": "Allow", "profile": "Private"}])
        self.assertEqual(len(probs), 1)
        self.assertIn("no inbound Allow rule", probs[0])
        self.assertIn("Public", probs[0])

    def test_combined_profiles_and_domain_name(self):
        rules = [{"action": "Allow", "profile": "Domain, Private"}]
        self.assertEqual(netcheck.firewall_problems(
            ["DomainAuthenticated", "Private"], rules), [])

    def test_block_beats_allow(self):
        rules = [{"action": "Allow", "profile": "Any"},
                 {"action": "Block", "profile": "Public"}]
        probs = netcheck.firewall_problems(["Public", "Private"], rules)
        self.assertEqual(len(probs), 1)
        self.assertIn("Block", probs[0])

    def test_no_rules(self):
        self.assertEqual(len(netcheck.firewall_problems(["Private"], [])), 1)

    def test_parse_ps_single_values_and_lists(self):
        prof, rules = netcheck.parse_ps(json.dumps(
            {"profiles": "Public",
             "rules": {"action": "Allow", "profile": "Any"}}))
        self.assertEqual(prof, ["Public"])
        self.assertEqual(rules, [{"action": "Allow", "profile": "Any"}])
        prof, rules = netcheck.parse_ps('{"profiles":[],"rules":[]}')
        self.assertEqual((prof, rules), ([], []))

    def test_lan_ipv4_excludes_loopback(self):
        for a in netcheck.lan_ipv4():
            self.assertFalse(a.startswith(("127.", "169.254.")))


class DiscoveryTests(unittest.IsolatedAsyncioTestCase):
    async def test_unicast_probe_gets_reply(self):
        class Stub:
            port = 12345
        loop = asyncio.get_running_loop()
        transport, _ = await loop.create_datagram_endpoint(
            lambda: DiscoveryProtocol(Stub()), local_addr=("127.0.0.1", 0))
        port = transport.get_extra_info("sockname")[1]
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setblocking(False)
        try:
            s.sendto(b'{"t":"discover","proto":1}\n', ("127.0.0.1", port))
            data = await asyncio.wait_for(loop.sock_recv(s, 1024), 2)
            msg = json.loads(data)
            self.assertEqual((msg["t"], msg["port"]), ("server", 12345))
            # junk is ignored, no reply
            s.sendto(b"hello", ("127.0.0.1", port))
            with self.assertRaises(asyncio.TimeoutError):
                await asyncio.wait_for(loop.sock_recv(s, 1024), 0.3)
        finally:
            s.close()
            transport.close()


if __name__ == "__main__":
    unittest.main()
