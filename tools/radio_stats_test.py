#!/usr/bin/env python3
"""Unit tests for radio_stats (no live radio required)."""

from __future__ import annotations

import socket
import threading
import unittest

from radio_stats import (
    delta,
    firmware_has_metrics,
    mgr_delta,
    parse_kv_line,
    read_via_manager,
    residual_checks,
    stages,
)


def fake_manager(reply: str) -> tuple[tuple[str, int], tuple[str, int], threading.Thread]:
    """One-shot manager console: answers the first command on console_out."""
    srv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    srv.bind(("127.0.0.1", 0))
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    probe.bind(("127.0.0.1", 0))
    out = probe.getsockname()
    probe.close()

    def serve() -> None:
        srv.settimeout(2.0)
        cmd, _ = srv.recvfrom(512)
        srv.sendto(reply.encode() if cmd == b"radio_stats" else b"NOK", out)
        srv.close()

    t = threading.Thread(target=serve)
    t.start()
    return out, srv.getsockname(), t


class RadioStatsTest(unittest.TestCase):
    def test_parse_tx_info(self) -> None:
        text = (
            "tx_info tx_queue_sz=2 in_flight=1 dropped_invalid_frame=0 "
            "dropped_tx_queue=3 dropped_wifi=1 ether_pkt=100 air_pkt=90 ts=5000000"
        )
        parsed = parse_kv_line(text)
        self.assertEqual(parsed["ether_pkt"], 100)
        self.assertEqual(parsed["air_pkt"], 90)
        self.assertEqual(parsed["ts"], 5000000)

    def test_parse_missing_fields(self) -> None:
        old = parse_kv_line("tx_info tx_queue_sz=0 in_flight=0")
        self.assertNotIn("ether_pkt", old)
        self.assertFalse(firmware_has_metrics({"tx": old, "rx": {}}))

    def test_u32_wrap_delta(self) -> None:
        before = {"tx": {"ether_pkt": 0xFFFFFFF0, "air_pkt": 0, "ts": 1000}, "rx": {}}
        after = {"tx": {"ether_pkt": 5, "air_pkt": 0, "ts": 2000}, "rx": {}}
        d = delta(before, after)
        self.assertEqual(d["tx"]["ether_pkt"], 21)
        self.assertEqual(d["ts_delta_us"], 1000)
        self.assertFalse(d["rebooted"])

    def test_reboot_detection(self) -> None:
        before = {"tx": {"ts": 9_000_000}, "rx": {"ts": 9_000_000}}
        after = {"tx": {"ts": 1000}, "rx": {"ts": 1000}}
        d = delta(before, after)
        self.assertTrue(d["rebooted"])

    def test_stages_and_residual(self) -> None:
        tx_d = {
            "tx": {
                "ether_pkt": 100,
                "air_pkt": 95,
                "dropped_invalid_frame": 1,
                "dropped_tx_queue": 2,
                "dropped_wifi": 2,
                "tx_queue_sz": 0,
                "in_flight": 0,
            },
            "rx": {},
        }
        rx_d = {
            "tx": {},
            "rx": {
                "air_pkt": 200,
                "dropped_filter_mismatched": 100,
                "dropped_rx_queue": 1,
                "dropped_no_peer": 0,
                "dropped_send_failed": 1,
                "ether_pkt": 98,
                "rx_queue_sz": 0,
            },
        }
        rows = stages(tx_d, rx_d, {"radio_tx_pkt": 100}, {"radio_rx_pkt": 98})
        self.assertEqual(rows[2][1], 95)
        self.assertEqual(rows[2][2], 100)
        res = residual_checks(tx_d, rx_d)
        self.assertEqual(res[0][1], 0)
        self.assertEqual(res[1][1], 0)

    def test_mgr_delta(self) -> None:
        self.assertEqual(
            mgr_delta({"radio_tx_pkt": 10}, {"radio_tx_pkt": 15})["radio_tx_pkt"],
            5,
        )

    def test_read_via_manager(self) -> None:
        bind, dest, t = fake_manager(
            "tx_info ether_pkt=10 air_pkt=9 ts=1\nrx_info ether_pkt=8 air_pkt=9 ts=2\n"
        )
        snap = read_via_manager(bind, dest)
        t.join()
        self.assertEqual(snap["tx"]["ether_pkt"], 10)
        self.assertEqual(snap["rx"]["ether_pkt"], 8)

    def test_read_via_manager_nok(self) -> None:
        bind, dest, t = fake_manager("NOK NOT_FOUND\n")
        with self.assertRaises(OSError):
            read_via_manager(bind, dest)
        t.join()


if __name__ == "__main__":
    unittest.main()
