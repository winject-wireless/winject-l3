#!/usr/bin/env python3
"""Stage-by-stage drop attribution for winject A↔B (USB air + radio + manager).

Snaps radio `tx_info` / `rx_info` and manager `gci` before/after each unidirectional phase,
captures winject MPDUs on a USB monitor iface, and prints where counts diverge.

    sudo ...  # monitor iface must already be in monitor mode on the radio channel
    python3 tools/drop_path_probe.py --a 192.168.253.9 --b 192.168.253.14 \\
        --mon wlx3c789537952a --kbps 10000 --duration 5
"""

from __future__ import annotations

import argparse
import os
import re
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

ADDR3 = bytes([0xCA, 0xFE, 0xBA, 0xBE, 0x12, 0x34])


def cons(ip: str, cmd: str, port: int = 22, timeout: float = 3.0) -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    try:
        s.sendto(cmd.encode(), (ip, port))
        d, _ = s.recvfrom(65535)
        return d.decode(errors="replace")
    finally:
        s.close()


def grab(text: str, key: str) -> str | None:
    m = re.search(rf"(?:^|\s){re.escape(key)}=([^\s]+)", text, re.M)
    return m.group(1) if m else None


def grab_int(text: str, key: str) -> int:
    v = grab(text, key)
    if v is None:
        return 0
    m = re.match(r"(-?\d+)", v)
    return int(m.group(1)) if m else 0


from radio_stats import (
    delta as radio_delta,
    firmware_has_metrics,
    format_stages,
    mgr_delta,
    read as radio_read,
    residual_checks,
    stages as radio_stages,
)


def mgr_gci(bind: tuple[str, int], dest: tuple[str, int], timeout: float = 2.0) -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    try:
        s.bind(bind)
        s.sendto(b"gci", dest)
        d, _ = s.recvfrom(65535)
        return d.decode(errors="replace")
    finally:
        s.close()


def parse_gci_stream(text: str, index: int) -> dict[str, int]:
    out = {
        "tx_pkt": 0,
        "rx_pkt": 0,
        "air_tx_pkt": 0,
        "drop_txq": 0,
        "rx_pkt_loss": 0,
    }
    for line in text.splitlines():
        if not line.startswith(f"stream-{index} "):
            continue
        for k in out:
            v = grab(line, k)
            if v is not None:
                out[k] = int(v)
        break
    # radio aggregate line
    for line in text.splitlines():
        if line.startswith("stream ") and "tx_pkt=" in line:
            out["radio_tx_pkt"] = grab_int(line, "tx_pkt")
            out["radio_rx_pkt"] = grab_int(line, "rx_pkt")
            break
    else:
        out["radio_tx_pkt"] = 0
        out["radio_rx_pkt"] = 0
    return out


def decode_bus(addr12: bytes) -> int:
    bits: list[int] = []
    for b in addr12:
        for i in range(8):
            bits.append((b >> i) & 1)
    pos = 1
    bus = 0
    for i in range(8):
        bus |= bits[pos] << i
        pos += 1
    return bus


def count_pcap_buses(path: Path) -> dict[int, int]:
    counts: dict[int, int] = {}
    with path.open("rb") as f:
        gh = f.read(24)
        if len(gh) < 24:
            return counts
        magic = struct.unpack("<I", gh[:4])[0]
        endian = "<" if magic == 0xA1B2C3D4 else ">"
        while True:
            h = f.read(16)
            if len(h) < 16:
                break
            _, _, incl, _ = struct.unpack(endian + "IIII", h)
            data = f.read(incl)
            if len(data) < 8:
                continue
            rlen = struct.unpack("<H", data[2:4])[0]
            body = data[rlen:]
            if len(body) < 22 or body[16:22] != ADDR3:
                continue
            bus = decode_bus(body[4:16])
            counts[bus] = counts.get(bus, 0) + 1
    return counts


@dataclass
class DropPathSnap:
    radio_tx: dict
    radio_rx: dict
    mgr_tx_stream: dict[str, int]
    mgr_rx_stream: dict[str, int]
    mgr_radio_tx_pkt: int
    mgr_radio_rx_pkt: int


@dataclass
class DropPathDelta:
    host_sent: int = 0
    host_recv: int = 0
    air: int = 0
    radio_tx_delta: dict = field(default_factory=dict)
    radio_rx_delta: dict = field(default_factory=dict)
    mgr_tx: dict[str, int] = field(default_factory=dict)
    mgr_rx: dict[str, int] = field(default_factory=dict)
    mgr_radio_tx_pkt: int = 0
    mgr_radio_rx_pkt: int = 0


def gci_radio_agg(gci: str) -> tuple[int, int]:
    for line in gci.splitlines():
        if line.startswith("stream ") and "tx_pkt=" in line:
            return grab_int(line, "tx_pkt"), grab_int(line, "rx_pkt")
    return 0, 0


def capture_drop_path_snap(
    tx_radio: str,
    rx_radio: str,
    mgr_tx_ports: tuple[tuple[str, int], tuple[str, int]],
    mgr_rx_ports: tuple[tuple[str, int], tuple[str, int]],
    mgr_tx_stream: int,
    mgr_rx_stream: int,
) -> DropPathSnap:
    ra = radio_read(tx_radio)
    rb = radio_read(rx_radio)
    gci_tx = mgr_gci(*mgr_tx_ports)
    gci_rx = mgr_gci(*mgr_rx_ports)
    st_tx = parse_gci_stream(gci_tx, mgr_tx_stream)
    st_rx = parse_gci_stream(gci_rx, mgr_rx_stream)
    mtx_tx, _ = gci_radio_agg(gci_tx)
    _, mrx_rx = gci_radio_agg(gci_rx)
    return DropPathSnap(
        radio_tx=ra,
        radio_rx=rb,
        mgr_tx_stream=st_tx,
        mgr_rx_stream=st_rx,
        mgr_radio_tx_pkt=mtx_tx,
        mgr_radio_rx_pkt=mrx_rx,
    )


def drop_path_delta(
    before: DropPathSnap,
    after: DropPathSnap,
    host_sent: int,
    host_recv: int,
    air: int = 0,
) -> DropPathDelta:
    mgr_keys = ("tx_pkt", "air_tx_pkt", "drop_txq", "rx_pkt", "rx_pkt_loss")
    mgr_tx_before = {
        "radio_tx_pkt": before.mgr_radio_tx_pkt,
        "radio_rx_pkt": 0,
    }
    mgr_tx_after = {
        "radio_tx_pkt": after.mgr_radio_tx_pkt,
        "radio_rx_pkt": 0,
    }
    mgr_rx_before = {
        "radio_tx_pkt": 0,
        "radio_rx_pkt": before.mgr_radio_rx_pkt,
    }
    mgr_rx_after = {
        "radio_tx_pkt": 0,
        "radio_rx_pkt": after.mgr_radio_rx_pkt,
    }
    return DropPathDelta(
        host_sent=host_sent,
        host_recv=host_recv,
        air=air,
        radio_tx_delta=radio_delta(before.radio_tx, after.radio_tx),
        radio_rx_delta=radio_delta(before.radio_rx, after.radio_rx),
        mgr_tx={
            k: after.mgr_tx_stream.get(k, 0) - before.mgr_tx_stream.get(k, 0)
            for k in mgr_keys
        },
        mgr_rx={
            k: after.mgr_rx_stream.get(k, 0) - before.mgr_rx_stream.get(k, 0)
            for k in mgr_keys
        },
        mgr_radio_tx_pkt=mgr_delta(mgr_tx_before, mgr_tx_after)["radio_tx_pkt"],
        mgr_radio_rx_pkt=mgr_delta(mgr_rx_before, mgr_rx_after)["radio_rx_pkt"],
    )


def print_drop_path_stages(label: str, pr: DropPathDelta) -> None:
    tx_d = pr.radio_tx_delta
    rx_d = pr.radio_rx_delta
    if tx_d.get("rebooted") or rx_d.get("rebooted"):
        print(f"\nwarning: {label} radio reboot during phase; stage table skipped")
        return
    if not firmware_has_metrics({"tx": tx_d.get("tx", {}), "rx": {}}):
        print(
            "\nradio drop counters unavailable (older firmware); "
            "host/mgr summary only"
        )
    else:
        mgr_s = {"radio_tx_pkt": pr.mgr_radio_tx_pkt}
        mgr_r = {"radio_rx_pkt": pr.mgr_radio_rx_pkt}
        rows = radio_stages(tx_d, rx_d, mgr_s, mgr_r)
        extras = [
            "",
            "-- host / manager (gci stream) --",
            f"  host→mgr_app          {pr.host_sent:7d} → {pr.mgr_tx['tx_pkt']:7d}   "
            f"drop {pr.host_sent - pr.mgr_tx['tx_pkt']:7d}",
            f"  mgr_udp→host          {pr.mgr_rx['rx_pkt']:7d} → {pr.host_recv:7d}   "
            f"drop {pr.mgr_rx['rx_pkt'] - pr.host_recv:7d}",
        ]
        if pr.air > 0:
            tx_air = tx_d.get("tx", {}).get("air_pkt", 0)
            extras.append(
                f"\n  USB monitor on-air: {pr.air}  (S air_pkt Δ={tx_air})"
            )
        print(
            format_stages(
                label,
                rows,
                residual_checks(tx_d, rx_d),
                extras,
            )
        )


@dataclass
class PhaseResult:
    label: str
    host_sent: int = 0
    host_recv: int = 0
    air: int = 0
    radio_tx_delta: dict = field(default_factory=dict)
    radio_rx_delta: dict = field(default_factory=dict)
    mgr_tx: dict[str, int] = field(default_factory=dict)
    mgr_rx: dict[str, int] = field(default_factory=dict)
    mgr_radio_tx_pkt: int = 0
    mgr_radio_rx_pkt: int = 0


class Listener:
    def __init__(self, port: int):
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.s.bind(("0.0.0.0", port))
        self.s.settimeout(0.2)
        self.n = 0
        self.stop = False
        self.t = threading.Thread(target=self.loop, daemon=True)
        self.t.start()

    def loop(self) -> None:
        while not self.stop:
            try:
                self.s.recvfrom(65535)
                self.n += 1
            except socket.timeout:
                continue
            except OSError:
                break

    def close(self) -> None:
        self.stop = True
        self.t.join(1)
        self.s.close()


def pace_send(dest: tuple[str, int], payload: bytes, kbps: float, duration: float) -> int:
    interval = (len(payload) * 8) / (kbps * 1000.0)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sent = 0
    t0 = time.monotonic()
    nxt = t0
    end = t0 + duration
    while time.monotonic() < end:
        sock.sendto(payload, dest)
        sent += 1
        nxt += interval
        sleep = nxt - time.monotonic()
        if sleep > 0:
            time.sleep(sleep)
        elif time.monotonic() > nxt + interval:
            nxt = time.monotonic()
    sock.close()
    return sent


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--a", default="192.168.253.9")
    p.add_argument("--b", default="192.168.253.14")
    p.add_argument("--host", default="192.168.253.106")
    p.add_argument("--mon", default="wlx3c789537952a")
    p.add_argument("--kbps", type=float, default=10000)
    p.add_argument("--duration", type=float, default=5.0)
    p.add_argument("--size", type=int, default=1400)
    p.add_argument("--channel", type=int, default=11)
    args = p.parse_args()

    manager = os.environ.get(
        "WINJECT_MANAGER",
        str(ROOT / "build_manager_arm" / "winject-manager"),
    )
    if not Path(manager).is_file():
        raise SystemExit(f"missing manager binary: {manager}")

    # Keep USB NIC in monitor on the radio channel (NM must not reclaim it).
    freq = {1: 2412, 2: 2417, 3: 2422, 4: 2427, 5: 2432, 6: 2437, 7: 2442,
            8: 2447, 9: 2452, 10: 2457, 11: 2462, 12: 2467, 13: 2472}.get(args.channel, 2462)
    for cmd in (
        ["sudo", "nmcli", "device", "set", args.mon, "managed", "no"],
        ["sudo", "ip", "link", "set", args.mon, "down"],
        ["sudo", "iw", "dev", args.mon, "set", "type", "monitor"],
        ["sudo", "ip", "link", "set", args.mon, "up"],
        ["sudo", "iw", "dev", args.mon, "set", "freq", str(freq), "HT20"],
    ):
        subprocess.run(cmd, check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    # PHY + upstreams
    for ip in (args.a, args.b):
        for c in (
            "set_mode STANDALONE",
            "set_domain 1234",
            f"set_channel {args.channel}",
            "set_modulation OFDM_24M",
            "set_tx_power 20",
            "set_cca_enabled 0",
        ):
            cons(ip, c)
    log_dir = Path(tempfile.mkdtemp(prefix="drop_path_"))
    print(f"logs {log_dir}")

    def write_conf(path: Path, device: str, fwd: int, cons_in: int, cons_out: int) -> None:
        if device.endswith(".9"):
            body = f"""
winject.device        = {device}
winject.local_ip      = {args.host}
winject.console       = 22
winject.channel       = {args.channel}
winject.modulation    = OFDM_24M
winject.power         = 20
winject.domain        = 1234
winject.max_rate_kbps = {int(args.kbps)}
winject.stats_sec     = 1
winject.skip_console  = 1
winject.forward_base  = {fwd}
manager.console_in    = 127.0.0.1:{cons_in}
manager.console_out   = 127.0.0.1:{cons_out}
upstream.size = 2
upstream-0.mode             = UDP_SERVER_FORWARDING
upstream-0.tx_bus           = b2
upstream-0.rx_bus           = a1
upstream-0.scheduler_budget = 65536
upstream-0.bind_address     = 127.0.0.1:29000
upstream-1.mode             = UDP_CLIENT_FORWARDING
upstream-1.tx_bus           = c3
upstream-1.rx_bus           = d4
upstream-1.scheduler_budget = 65536
upstream-1.connect_address  = 127.0.0.1:9001
"""
        else:
            body = f"""
winject.device        = {device}
winject.local_ip      = {args.host}
winject.console       = 22
winject.channel       = {args.channel}
winject.modulation    = OFDM_24M
winject.power         = 20
winject.domain        = 1234
winject.max_rate_kbps = {int(args.kbps)}
winject.stats_sec     = 1
winject.skip_console  = 1
winject.forward_base  = {fwd}
manager.console_in    = 127.0.0.1:{cons_in}
manager.console_out   = 127.0.0.1:{cons_out}
upstream.size = 2
upstream-0.mode             = UDP_CLIENT_FORWARDING
upstream-0.tx_bus           = a1
upstream-0.rx_bus           = b2
upstream-0.scheduler_budget = 65536
upstream-0.connect_address  = 127.0.0.1:9002
upstream-1.mode             = UDP_SERVER_FORWARDING
upstream-1.tx_bus           = d4
upstream-1.rx_bus           = c3
upstream-1.scheduler_budget = 65536
upstream-1.bind_address     = 127.0.0.1:29001
"""
        path.write_text(body)

    conf_a = log_dir / "a.conf"
    conf_b = log_dir / "b.conf"
    # A gci: send to :2400, reply to :2401; B: :2410/:2411
    write_conf(conf_a, args.a, 9210, 2400, 2401)
    write_conf(conf_b, args.b, 9210, 2410, 2411)

    procs: list[subprocess.Popen] = []
    for conf, logn in ((conf_a, "a.log"), (conf_b, "b.log")):
        logf = open(log_dir / logn, "w")
        procs.append(
            subprocess.Popen(
                [manager, str(conf)],
                stdout=logf,
                stderr=subprocess.STDOUT,
                cwd=str(ROOT),
            )
        )
    time.sleep(1.5)
    for lab, path in (("A", log_dir / "a.log"), ("B", log_dir / "b.log")):
        txt = path.read_text()
        if "manager running" not in txt:
            print(f"manager {lab} failed to start:\n{txt[-500:]}")
            for pr in procs:
                pr.send_signal(signal.SIGTERM)
            return 1

    for radio_ip, log_name in ((args.a, "a.log"), (args.b, "b.log")):
        subprocess.run(
            [
                sys.executable,
                str(ROOT / "tools/configure_manager_ci.py"),
                "--radio",
                radio_ip,
                "--host",
                args.host,
                "--log",
                str(log_dir / log_name),
                "--quiet",
            ],
            check=False,
            cwd=str(ROOT),
        )

    payload = bytes([0xCD]) * args.size
    results: list[PhaseResult] = []

    def run_phase(
        label: str,
        send_addr: tuple[str, int],
        listen_port: int,
        bus: int,
        tx_radio: str,
        rx_radio: str,
        mgr_tx_ports: tuple[tuple[str, int], tuple[str, int]],
        mgr_rx_ports: tuple[tuple[str, int], tuple[str, int]],
        mgr_tx_stream: int,
        mgr_rx_stream: int,
    ) -> None:
        pcap = log_dir / f"{label.replace('->', '_').replace('>', '')}.pcap"
        # start tcpdump (filter as one expression; recreate file as root)
        subprocess.run(["sudo", "rm", "-f", str(pcap)], check=False)
        td = subprocess.Popen(
            [
                "sudo",
                "tcpdump",
                "-i",
                args.mon,
                "-w",
                str(pcap),
                "-s",
                "256",
                "wlan addr3 ca:fe:ba:be:12:34",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        time.sleep(0.5)

        snap0 = capture_drop_path_snap(
            tx_radio,
            rx_radio,
            mgr_tx_ports,
            mgr_rx_ports,
            mgr_tx_stream,
            mgr_rx_stream,
        )

        lis = Listener(listen_port)
        time.sleep(0.1)
        sent = pace_send(send_addr, payload, args.kbps, args.duration)
        t_d = time.monotonic()
        while time.monotonic() - t_d < 2.0 and lis.n < sent:
            time.sleep(0.05)
        time.sleep(0.3)
        recv = lis.n
        lis.close()

        td.send_signal(signal.SIGINT)
        try:
            td.wait(timeout=3)
        except subprocess.TimeoutExpired:
            td.kill()

        snap1 = capture_drop_path_snap(
            tx_radio,
            rx_radio,
            mgr_tx_ports,
            mgr_rx_ports,
            mgr_tx_stream,
            mgr_rx_stream,
        )

        # tcpdump may leave root-owned pcap; make readable
        subprocess.run(["sudo", "chmod", "a+r", str(pcap)], check=False)
        buses = count_pcap_buses(pcap) if pcap.is_file() else {}
        air = buses.get(bus, 0)
        air_all = sum(buses.values())

        dp = drop_path_delta(snap0, snap1, sent, recv, air=air)
        pr = PhaseResult(
            label=label,
            host_sent=dp.host_sent,
            host_recv=dp.host_recv,
            air=dp.air,
            radio_tx_delta=dp.radio_tx_delta,
            radio_rx_delta=dp.radio_rx_delta,
            mgr_tx=dp.mgr_tx,
            mgr_rx=dp.mgr_rx,
            mgr_radio_tx_pkt=dp.mgr_radio_tx_pkt,
            mgr_radio_rx_pkt=dp.mgr_radio_rx_pkt,
        )
        results.append(pr)

        loss_pct = 100.0 * (1.0 - pr.host_recv / pr.host_sent) if pr.host_sent else 0.0
        print(f"\n=== {label} offer {args.kbps:.0f} kbps {args.duration:.1f}s  loss={loss_pct:.1f}% ===")
        rtx = pr.radio_tx_delta.get("tx", {})
        rrx = pr.radio_rx_delta.get("rx", {})
        print(f"host_sent          {pr.host_sent}")
        print(f"mgr_tx app_rx      {pr.mgr_tx['tx_pkt']}   (stream ingest)")
        print(f"mgr_tx drop_txq    {pr.mgr_tx['drop_txq']}   (silent txq overflow)")
        print(f"mgr wifi_udp TX    {pr.mgr_radio_tx_pkt}   (MPDUs to radio)")
        print(f"radio ether_pkt Δ  {rtx.get('ether_pkt', 0)}")
        print(f"radio air_pkt Δ    {rtx.get('air_pkt', 0)}")
        print(f"radio drop_invalid {rtx.get('dropped_invalid_frame', 0)}")
        print(f"radio drop_tx_q    {rtx.get('dropped_tx_queue', 0)}")
        print(f"radio drop_wifi    {rtx.get('dropped_wifi', 0)}")
        print(f"USB air bus {bus:#x}  {pr.air}   (all winject {air_all})")
        print(f"peer air_pkt Δ     {rrx.get('air_pkt', 0)}")
        print(f"peer drop_filter   {rrx.get('dropped_filter_mismatched', 0)}")
        print(f"peer drop_rx_q     {rrx.get('dropped_rx_queue', 0)}")
        print(f"peer drop_no_peer  {rrx.get('dropped_no_peer', 0)}")
        print(f"peer drop_send     {rrx.get('dropped_send_failed', 0)}")
        print(f"peer ether_pkt Δ   {rrx.get('ether_pkt', 0)}")
        print(f"mgr wifi_udp RX    {pr.mgr_radio_rx_pkt}")
        print(f"mgr_rx stream_rx   {pr.mgr_rx['rx_pkt']}")
        print(f"mgr_rx seq_loss    {pr.mgr_rx['rx_pkt_loss']}")
        print(f"host_recv          {pr.host_recv}")
        try:
            rx_st = cons(rx_radio, "wifi_rx")
            m = re.search(r"rssi=(-?\d+) snr=(-?\d+)", rx_st)
            if m:
                print(f"peer rssi/snr      {m.group(1)} dBm / {m.group(2)} dB")
            else:
                print("peer rssi/snr      - (no winject RX yet)")
        except OSError:
            print("peer rssi/snr      - (wifi_rx query failed)")
        if td.stderr:
            err = td.stderr.read().decode(errors="replace").strip()
            if err and "listening" not in err.lower():
                print(f"tcpdump: {err[-300:]}")

        print_drop_path_stages(label, dp)

    # A→B: send 29000, listen 9002, bus b2, mgr A stream0, mgr B stream0
    run_phase(
        "A->B",
        ("127.0.0.1", 29000),
        9002,
        0xB2,
        args.a,
        args.b,
        (("127.0.0.1", 2401), ("127.0.0.1", 2400)),
        (("127.0.0.1", 2411), ("127.0.0.1", 2410)),
        0,
        0,
    )
    # B→A: send 29001, listen 9001, bus d4, mgr B stream1, mgr A stream1
    run_phase(
        "B->A",
        ("127.0.0.1", 29001),
        9001,
        0xD4,
        args.b,
        args.a,
        (("127.0.0.1", 2411), ("127.0.0.1", 2410)),
        (("127.0.0.1", 2401), ("127.0.0.1", 2400)),
        1,
        1,
    )

    for pr in procs:
        pr.send_signal(signal.SIGTERM)
    for pr in procs:
        pr.wait(timeout=5)

    print(f"\ndone. artifacts in {log_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
