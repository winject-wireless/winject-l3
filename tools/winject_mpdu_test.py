#!/usr/bin/env python3
"""Unit tests for winject_mpdu LC air-RX gap tracking."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from winject_mpdu import LcAirRxTracker, lc_write  # noqa: E402


class LcAirRxTrackerTest(unittest.TestCase):
    def test_first_slot_no_gap_event(self) -> None:
        t = LcAirRxTracker(bus=0xB2)
        self.assertTrue(t.accept_slot(lc_write(0xB2, 10) + b"x"))
        self.assertEqual(t.gap_events, [])
        self.assertEqual(t.snapshot(), (1, 0))

    def test_seq_jump_records_one_gap_event(self) -> None:
        t = LcAirRxTracker(bus=0xB2)
        t.accept_slot(lc_write(0xB2, 0) + b"a")
        with mock.patch("winject_mpdu.time.monotonic", return_value=1.0):
            t.accept_slot(lc_write(0xB2, 3) + b"b")
        self.assertEqual(len(t.gap_events), 1)
        self.assertEqual(t.gap_events[0].missed_slots, 2)
        self.assertEqual(t.snapshot(), (2, 2))

    def test_in_order_second_slot_no_gap(self) -> None:
        t = LcAirRxTracker(bus=0xB2)
        t.accept_slot(lc_write(0xB2, 5) + b"a")
        t.accept_slot(lc_write(0xB2, 6) + b"b")
        self.assertEqual(t.gap_events, [])
        self.assertEqual(t.snapshot(), (2, 0))


class MissedPerGapEventTest(unittest.TestCase):
    def test_split(self) -> None:
        from air_rx_loss_char import _missed_per_gap_event

        self.assertEqual(_missed_per_gap_event(1, 5), [5])
        self.assertEqual(sum(_missed_per_gap_event(3, 5)), 5)
        self.assertEqual(len(_missed_per_gap_event(3, 5)), 3)


if __name__ == "__main__":
    unittest.main()
