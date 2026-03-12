#!/usr/bin/env python3
"""
Comprehensive load test for the Counter-Drone TCP server.

Simulates 1–1000 drones connecting simultaneously, each running one of six
behavioural profiles.  A Rich terminal dashboard displays real-time
statistics including packets sent, bytes transferred, connection status,
per-profile breakdowns, and error counts.

Usage
-----
    # Start the C++ server first:
    ./build/counter_drone_server 9000

    # Basic run (10 drones, 30 s, 20 pkt/s each):
    python3 tests/drone_load_test.py

    # Custom run:
    python3 tests/drone_load_test.py --drones 50 --duration 60 --rate 40 --port 9000

Dependencies
------------
    pip install rich
"""

from __future__ import annotations

import argparse
import asyncio
import collections
import math
import random
import struct
import sys
import time
from dataclasses import dataclass, field
from enum import Enum, auto
from typing import Optional

try:
    from rich.console import Console
    from rich.layout import Layout
    from rich.live import Live
    from rich.panel import Panel
    from rich.progress import BarColumn, Progress, TextColumn, TimeElapsedColumn
    from rich.table import Table
    from rich.text import Text
except ImportError:
    print("ERROR: 'rich' library is required.  Install with:  pip install rich")
    sys.exit(1)


# ────────────────────────── Wire-protocol constants ──────────────────────────

PACKET_HEADER = 0xAA55
HEADER_SIZE = 2
LENGTH_SIZE = 2
CRC_SIZE = 2


# ─────────────────────────── CRC-16 / MODBUS ────────────────────────────────

def crc16_modbus(data: bytes) -> int:
    """CRC-16/MODBUS — matches the C++ server implementation exactly.

    Initial value 0xFFFF, polynomial 0xA001, LSB-first bit processing.
    """
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc & 0xFFFF


# ─────────────────────── Packet serialisation ────────────────────────────────

def serialize_payload(drone_id: str, lat: float, lon: float,
                      alt: float, speed: float, timestamp: int) -> bytes:
    """Serialize a telemetry payload in the server's big-endian wire format.

    Layout: id_len(u16) + drone_id(chars) + lat(f64) + lon(f64) +
            alt(f64) + speed(f64) + timestamp(u64).  All multi-byte
            fields are big-endian.
    """
    id_bytes = drone_id.encode("ascii")
    return struct.pack(f">H{len(id_bytes)}sddddQ",
                       len(id_bytes), id_bytes,
                       lat, lon, alt, speed, timestamp)


def build_packet(drone_id: str, lat: float, lon: float,
                 alt: float, speed: float, timestamp: int,
                 corrupt_crc: bool = False) -> bytes:
    """Build a complete wire packet:  HEADER(2) + LENGTH(2) + PAYLOAD(N) + CRC(2).

    When *corrupt_crc* is True the CRC is intentionally wrong so the server
    rejects the packet (used by the 'corrupted' drone profile).
    """
    payload = serialize_payload(drone_id, lat, lon, alt, speed, timestamp)

    # Header + length (big-endian)
    header_bytes = struct.pack(">H", PACKET_HEADER)
    length_bytes = struct.pack(">H", len(payload))

    # CRC is computed over header + length + payload (raw wire bytes)
    raw = header_bytes + length_bytes + payload
    crc = crc16_modbus(raw)

    if corrupt_crc:
        crc ^= 0xBEEF  # flip bits to guarantee a mismatch

    crc_bytes = struct.pack(">H", crc)
    return raw + crc_bytes


# ────────────────────────── Drone profiles ───────────────────────────────────

class ProfileType(Enum):
    NORMAL = auto()
    THRESHOLD = auto()
    CORRUPTED = auto()
    CHURNER = auto()
    RAMPER = auto()
    TRICKLER = auto()


PROFILE_NAMES = {
    ProfileType.NORMAL:    "Normal",
    ProfileType.THRESHOLD: "Threshold",
    ProfileType.CORRUPTED: "Corrupted",
    ProfileType.CHURNER:   "Churner",
    ProfileType.RAMPER:    "Ramper",
    ProfileType.TRICKLER:  "Trickler",
}

PROFILE_COLOURS = {
    ProfileType.NORMAL:    "green",
    ProfileType.THRESHOLD: "yellow",
    ProfileType.CORRUPTED: "red",
    ProfileType.RAMPER:    "cyan",
    ProfileType.CHURNER:   "magenta",
    ProfileType.TRICKLER:  "blue",
}


# ────────────────────────── Statistics ───────────────────────────────────────

@dataclass
class Stats:
    """Shared mutable statistics updated by every drone coroutine."""
    packets_sent: int = 0
    bytes_sent: int = 0
    connections_opened: int = 0
    connections_closed: int = 0
    corrupt_packets_sent: int = 0
    connect_failures: int = 0
    send_errors: int = 0
    per_profile: dict[ProfileType, int] = field(default_factory=lambda: {p: 0 for p in ProfileType})
    active_drones: int = 0
    lock: asyncio.Lock = field(default_factory=asyncio.Lock)

    # Rolling-window PPS tracking (3-second window)
    _pps_window: collections.deque = field(default_factory=lambda: collections.deque())
    _pps_window_seconds: float = 3.0
    peak_pps: float = 0.0

    @property
    def instant_pps(self) -> float:
        """Compute packets-per-second over the rolling window."""
        now = time.time()
        # Prune old entries
        cutoff = now - self._pps_window_seconds
        while self._pps_window and self._pps_window[0] < cutoff:
            self._pps_window.popleft()
        count = len(self._pps_window)
        return count / self._pps_window_seconds if self._pps_window_seconds > 0 else 0

    async def record_packet(self, profile: ProfileType, nbytes: int, corrupt: bool = False):
        async with self.lock:
            self.packets_sent += 1
            self.bytes_sent += nbytes
            self.per_profile[profile] += 1
            if corrupt:
                self.corrupt_packets_sent += 1
            self._pps_window.append(time.time())
            # Update peak PPS
            current = self.instant_pps
            if current > self.peak_pps:
                self.peak_pps = current

    async def record_connect(self):
        async with self.lock:
            self.connections_opened += 1
            self.active_drones += 1

    async def record_disconnect(self):
        async with self.lock:
            self.connections_closed += 1
            self.active_drones -= 1

    async def record_connect_failure(self):
        async with self.lock:
            self.connect_failures += 1

    async def record_send_error(self):
        async with self.lock:
            self.send_errors += 1


# ───────────────────────── Drone coroutines ──────────────────────────────────

async def tcp_connect(host: str, port: int) -> Optional[tuple[asyncio.StreamReader, asyncio.StreamWriter]]:
    """Open a TCP connection; returns None on failure."""
    try:
        reader, writer = await asyncio.open_connection(host, port)
        return reader, writer
    except OSError:
        return None


def random_telemetry(drone_id: str, *, force_threshold: bool = False) -> dict:
    """Generate randomised telemetry values.

    When *force_threshold* is True at least one threshold is exceeded
    (altitude > 120 m or speed > 50 m/s).
    """
    lat = random.uniform(29.0, 34.0)
    lon = random.uniform(34.0, 36.0)
    alt = random.uniform(0.0, 100.0)
    speed = random.uniform(0.0, 40.0)
    ts = int(time.time())

    if force_threshold:
        if random.random() < 0.5:
            alt = random.uniform(121.0, 500.0)
        else:
            speed = random.uniform(51.0, 200.0)

    return dict(drone_id=drone_id, lat=lat, lon=lon, alt=alt, speed=speed, timestamp=ts)


async def send_packet(writer: asyncio.StreamWriter, data: bytes) -> bool:
    """Write *data* to the stream; returns False on broken pipe."""
    try:
        writer.write(data)
        await writer.drain()
        return True
    except (ConnectionError, OSError):
        return False


# ── Individual profile implementations ───────────────────────────────────────

async def run_normal(drone_id: str, host: str, port: int,
                     rate: float, end_time: float, stats: Stats):
    """Normal: steady stream of valid packets at the configured rate."""
    conn = await tcp_connect(host, port)
    if conn is None:
        await stats.record_connect_failure()
        return
    _, writer = conn
    await stats.record_connect()
    interval = 1.0 / rate
    try:
        while time.time() < end_time:
            t = random_telemetry(drone_id)
            pkt = build_packet(**t)
            if not await send_packet(writer, pkt):
                await stats.record_send_error()
                break
            await stats.record_packet(ProfileType.NORMAL, len(pkt))
            await asyncio.sleep(interval)
    finally:
        writer.close()
        try:
            await writer.wait_closed()
        except Exception:
            pass
        await stats.record_disconnect()


async def run_threshold(drone_id: str, host: str, port: int,
                        rate: float, end_time: float, stats: Stats):
    """Threshold violator: every packet exceeds altitude or speed limits."""
    conn = await tcp_connect(host, port)
    if conn is None:
        await stats.record_connect_failure()
        return
    _, writer = conn
    await stats.record_connect()
    interval = 1.0 / rate
    try:
        while time.time() < end_time:
            t = random_telemetry(drone_id, force_threshold=True)
            pkt = build_packet(**t)
            if not await send_packet(writer, pkt):
                await stats.record_send_error()
                break
            await stats.record_packet(ProfileType.THRESHOLD, len(pkt))
            await asyncio.sleep(interval)
    finally:
        writer.close()
        try:
            await writer.wait_closed()
        except Exception:
            pass
        await stats.record_disconnect()


async def run_corrupted(drone_id: str, host: str, port: int,
                        rate: float, end_time: float, stats: Stats):
    """Corrupted: ~20 % of packets have bad CRC."""
    conn = await tcp_connect(host, port)
    if conn is None:
        await stats.record_connect_failure()
        return
    _, writer = conn
    await stats.record_connect()
    interval = 1.0 / rate
    try:
        while time.time() < end_time:
            corrupt = random.random() < 0.20
            t = random_telemetry(drone_id)
            pkt = build_packet(**t, corrupt_crc=corrupt)
            if not await send_packet(writer, pkt):
                await stats.record_send_error()
                break
            await stats.record_packet(ProfileType.CORRUPTED, len(pkt), corrupt)
            await asyncio.sleep(interval)
    finally:
        writer.close()
        try:
            await writer.wait_closed()
        except Exception:
            pass
        await stats.record_disconnect()


async def run_churner(drone_id: str, host: str, port: int,
                      rate: float, end_time: float, stats: Stats):
    """Churner: repeatedly connects, sends 3–10 packets, then disconnects."""
    interval = 1.0 / rate
    while time.time() < end_time:
        conn = await tcp_connect(host, port)
        if conn is None:
            await stats.record_connect_failure()
            await asyncio.sleep(1.0)
            continue
        _, writer = conn
        await stats.record_connect()
        burst = random.randint(3, 10)
        try:
            for _ in range(burst):
                if time.time() >= end_time:
                    break
                t = random_telemetry(drone_id)
                pkt = build_packet(**t)
                if not await send_packet(writer, pkt):
                    await stats.record_send_error()
                    break
                await stats.record_packet(ProfileType.CHURNER, len(pkt))
                await asyncio.sleep(interval)
        finally:
            writer.close()
            try:
                await writer.wait_closed()
            except Exception:
                pass
            await stats.record_disconnect()
        # Brief pause between connection cycles
        await asyncio.sleep(random.uniform(0.5, 2.0))


async def run_ramper(drone_id: str, host: str, port: int,
                     rate: float, end_time: float, stats: Stats):
    """Ramper: waits a random delay, then ramps from 1 pkt/s up to the target rate."""
    total_duration = end_time - time.time()
    if total_duration <= 0:
        return
    # Wait 10–50 % of total duration before starting
    startup_delay = random.uniform(0.1, 0.5) * total_duration
    await asyncio.sleep(startup_delay)

    conn = await tcp_connect(host, port)
    if conn is None:
        await stats.record_connect_failure()
        return
    _, writer = conn
    await stats.record_connect()

    remaining = end_time - time.time()
    if remaining <= 0:
        writer.close()
        try:
            await writer.wait_closed()
        except Exception:
            pass
        await stats.record_disconnect()
        return

    try:
        elapsed_in_ramp = 0.0
        ramp_time = remaining * 0.6  # spend 60 % ramping, 40 % at full speed
        while time.time() < end_time:
            if ramp_time > 0 and elapsed_in_ramp < ramp_time:
                fraction = min(elapsed_in_ramp / ramp_time, 1.0)
                current_rate = max(1.0, fraction * rate)
            else:
                current_rate = rate

            interval = 1.0 / current_rate
            t = random_telemetry(drone_id)
            pkt = build_packet(**t)
            if not await send_packet(writer, pkt):
                await stats.record_send_error()
                break
            await stats.record_packet(ProfileType.RAMPER, len(pkt))
            await asyncio.sleep(interval)
            elapsed_in_ramp += interval
    finally:
        writer.close()
        try:
            await writer.wait_closed()
        except Exception:
            pass
        await stats.record_disconnect()


async def run_trickler(drone_id: str, host: str, port: int,
                       rate: float, end_time: float, stats: Stats):
    """Trickler: sends valid packets byte-by-byte with small delays.

    This stresses the streaming parser's ability to reassemble
    fragmented TCP streams.
    """
    conn = await tcp_connect(host, port)
    if conn is None:
        await stats.record_connect_failure()
        return
    _, writer = conn
    await stats.record_connect()
    interval = 1.0 / max(rate * 0.5, 1.0)  # slower effective rate
    try:
        while time.time() < end_time:
            t = random_telemetry(drone_id)
            pkt = build_packet(**t)
            # Send byte-by-byte (or in small chunks of 1–4 bytes)
            offset = 0
            ok = True
            while offset < len(pkt):
                chunk_size = random.randint(1, 4)
                chunk = pkt[offset:offset + chunk_size]
                if not await send_packet(writer, chunk):
                    ok = False
                    break
                offset += chunk_size
                # Tiny random delay between fragments
                await asyncio.sleep(random.uniform(0.001, 0.01))
            if not ok:
                await stats.record_send_error()
                break
            await stats.record_packet(ProfileType.TRICKLER, len(pkt))
            await asyncio.sleep(interval)
    finally:
        writer.close()
        try:
            await writer.wait_closed()
        except Exception:
            pass
        await stats.record_disconnect()


PROFILE_RUNNERS = {
    ProfileType.NORMAL:    run_normal,
    ProfileType.THRESHOLD: run_threshold,
    ProfileType.CORRUPTED: run_corrupted,
    ProfileType.CHURNER:   run_churner,
    ProfileType.RAMPER:    run_ramper,
    ProfileType.TRICKLER:  run_trickler,
}


# ─────────────────────── Rich Dashboard ──────────────────────────────────────

def build_dashboard(stats: Stats, args: argparse.Namespace,
                    start_time: float, profile_assignments: dict[str, ProfileType]) -> Layout:
    """Construct a Rich Layout snapshot for the current statistics."""
    elapsed = time.time() - start_time
    avg_pps = stats.packets_sent / elapsed if elapsed > 0 else 0
    bps = stats.bytes_sent / elapsed if elapsed > 0 else 0
    current_pps = stats.instant_pps

    # ── Header ──
    header_text = Text.assemble(
        ("COUNTER-DRONE LOAD TEST", "bold white on blue"),
        ("  |  ", "dim"),
        (f"{args.drones} drones", "bold cyan"),
        ("  |  ", "dim"),
        (f"{args.host}:{args.port}", "bold green"),
        ("  |  ", "dim"),
        (f"Duration: {args.duration}s", "bold yellow"),
    )
    header = Panel(header_text, style="bold blue", height=3)

    # ── Global stats table ──
    global_table = Table(title="Global Statistics", expand=True, show_lines=True)
    global_table.add_column("Metric", style="bold")
    global_table.add_column("Value", justify="right")
    global_table.add_row("Elapsed", f"{elapsed:.1f} s")
    global_table.add_row("Packets Sent", f"{stats.packets_sent:,}")
    global_table.add_row("Bytes Sent", _human_bytes(stats.bytes_sent))
    global_table.add_row("[bold cyan]Instant PPS[/]", f"[bold cyan]{current_pps:,.1f} pkt/s[/]")
    global_table.add_row("Avg PPS", f"{avg_pps:,.1f} pkt/s")
    global_table.add_row("[bold yellow]Peak PPS[/]", f"[bold yellow]{stats.peak_pps:,.1f} pkt/s[/]")
    global_table.add_row("Bandwidth", f"{_human_bytes(int(bps))}/s")
    global_table.add_row("Active Drones", f"[bold green]{stats.active_drones}[/]")
    global_table.add_row("Connections Opened", f"{stats.connections_opened}")
    global_table.add_row("Connections Closed", f"{stats.connections_closed}")
    global_table.add_row("Corrupt Packets", f"[bold red]{stats.corrupt_packets_sent}[/]")
    global_table.add_row("Connect Failures", f"[bold red]{stats.connect_failures}[/]")
    global_table.add_row("Send Errors", f"[bold red]{stats.send_errors}[/]")

    # ── Per-profile breakdown ──
    profile_table = Table(title="Profile Breakdown", expand=True, show_lines=True)
    profile_table.add_column("Profile", style="bold")
    profile_table.add_column("Count", justify="center")
    profile_table.add_column("Packets", justify="right")
    profile_table.add_column("Ratio", justify="right")

    # Count drones per profile
    profile_drone_count: dict[ProfileType, int] = {p: 0 for p in ProfileType}
    for p in profile_assignments.values():
        profile_drone_count[p] += 1

    total = max(stats.packets_sent, 1)
    for p in ProfileType:
        count = profile_drone_count.get(p, 0)
        pkts = stats.per_profile.get(p, 0)
        ratio = (pkts / total) * 100 if total > 0 else 0
        colour = PROFILE_COLOURS[p]
        profile_table.add_row(
            f"[{colour}]{PROFILE_NAMES[p]}[/]",
            str(count),
            f"{pkts:,}",
            f"{ratio:.1f}%",
        )

    # ── Drone roster (first 30 for space) ──
    drone_table = Table(title="Drone Roster (sample)", expand=True)
    drone_table.add_column("#", justify="center", width=4)
    drone_table.add_column("ID", style="bold")
    drone_table.add_column("Profile")
    shown = list(profile_assignments.items())[:30]
    for idx, (did, prof) in enumerate(shown, 1):
        colour = PROFILE_COLOURS[prof]
        drone_table.add_row(str(idx), did, f"[{colour}]{PROFILE_NAMES[prof]}[/]")
    if len(profile_assignments) > 30:
        drone_table.add_row("...", f"+ {len(profile_assignments) - 30} more", "")

    # ── Progress bar ──
    remaining = max(0, args.duration - elapsed)
    pct = min(elapsed / args.duration, 1.0) * 100
    progress_text = Text.assemble(
        ("Progress: ", "bold"),
        (f"{pct:.0f}%", "bold green"),
        (f"  ({remaining:.0f}s remaining)", "dim"),
    )

    bar_width = 40
    filled = int(bar_width * min(pct / 100, 1.0))
    bar = f"[bold green]{'█' * filled}[/][dim]{'░' * (bar_width - filled)}[/]"
    progress_panel = Panel(Text.from_markup(f"{progress_text}\n{bar}"), title="Progress", height=5)

    # ── Compose layout ──
    layout = Layout()
    layout.split_column(
        Layout(header, name="header", size=3),
        Layout(name="body"),
        Layout(progress_panel, name="footer", size=5),
    )
    layout["body"].split_row(
        Layout(Panel(global_table, title="Stats"), name="left"),
        Layout(name="right"),
    )
    layout["right"].split_column(
        Layout(Panel(profile_table, title="Profiles"), name="profiles"),
        Layout(Panel(drone_table, title="Drones"), name="drones"),
    )
    return layout


def _human_bytes(n: int) -> str:
    """Format a byte count into a human-readable string."""
    for unit in ("B", "KB", "MB", "GB"):
        if abs(n) < 1024:
            return f"{n:,.1f} {unit}"
        n = int(n / 1024)
    return f"{n:,.1f} TB"


# ─────────────────────── Orchestrator ────────────────────────────────────────

def assign_profiles(num_drones: int) -> dict[str, ProfileType]:
    """Round-robin assign profiles to drone IDs."""
    profiles = list(ProfileType)
    assignments: dict[str, ProfileType] = {}
    for i in range(num_drones):
        drone_id = f"DRONE-{i + 1:04d}"
        assignments[drone_id] = profiles[i % len(profiles)]
    return assignments


async def run_load_test(args: argparse.Namespace):
    """Main entry: spawn all drone coroutines and the dashboard refresh loop."""
    stats = Stats()
    profile_assignments = assign_profiles(args.drones)
    start_time = time.time()
    end_time = start_time + args.duration

    console = Console()

    # Spawn drone tasks
    tasks: list[asyncio.Task] = []
    for drone_id, profile in profile_assignments.items():
        runner = PROFILE_RUNNERS[profile]
        task = asyncio.create_task(
            runner(drone_id, args.host, args.port, args.rate, end_time, stats)
        )
        tasks.append(task)

    # Dashboard refresh loop
    with Live(
        build_dashboard(stats, args, start_time, profile_assignments),
        console=console,
        refresh_per_second=4,
        screen=True,
    ) as live:
        while time.time() < end_time:
            live.update(build_dashboard(stats, args, start_time, profile_assignments))
            await asyncio.sleep(0.25)
        # Final update
        live.update(build_dashboard(stats, args, start_time, profile_assignments))

    # Wait for all drones to finish (they should already be done or nearly)
    await asyncio.gather(*tasks, return_exceptions=True)

    # ── Final summary ──
    elapsed = time.time() - start_time
    avg_pps = stats.packets_sent / elapsed if elapsed > 0 else 0

    console.print()
    console.rule("[bold blue]Load Test Complete[/]")
    summary = Table(title="Final Summary", show_lines=True)
    summary.add_column("Metric", style="bold")
    summary.add_column("Value", justify="right")
    summary.add_row("Total Duration", f"{elapsed:.1f} s")
    summary.add_row("Total Drones", str(args.drones))
    summary.add_row("Total Packets", f"{stats.packets_sent:,}")
    summary.add_row("Total Bytes", _human_bytes(stats.bytes_sent))
    summary.add_row("Avg Throughput", f"{avg_pps:,.1f} pkt/s")
    summary.add_row("[bold yellow]Peak Throughput[/]", f"[bold yellow]{stats.peak_pps:,.1f} pkt/s[/]")
    summary.add_row("Connections Opened", str(stats.connections_opened))
    summary.add_row("Connections Closed", str(stats.connections_closed))
    summary.add_row("Corrupt Packets", f"[bold red]{stats.corrupt_packets_sent}[/]")
    summary.add_row("Connect Failures", f"[bold red]{stats.connect_failures}[/]")
    summary.add_row("Send Errors", f"[bold red]{stats.send_errors}[/]")
    console.print(summary)

    # Per-profile final summary
    profile_summary = Table(title="Per-Profile Packets", show_lines=True)
    profile_summary.add_column("Profile", style="bold")
    profile_summary.add_column("Packets", justify="right")
    for p in ProfileType:
        colour = PROFILE_COLOURS[p]
        profile_summary.add_row(
            f"[{colour}]{PROFILE_NAMES[p]}[/]",
            f"{stats.per_profile[p]:,}",
        )
    console.print(profile_summary)
    console.print()


# ──────────────────────────── CLI ────────────────────────────────────────────

def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Counter-Drone TCP Server — Comprehensive Load Test",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
Drone profiles (assigned round-robin):
  1. Normal      – Steady stream of valid packets
  2. Threshold   – Every packet exceeds altitude/speed limits
  3. Corrupted   – ~20% of packets have bad CRC
  4. Churner     – Rapid connect / send burst / disconnect cycles
  5. Ramper      – Delayed start, gradual rate increase
  6. Trickler    – Byte-by-byte fragmented packet delivery

Example:
  python3 tests/drone_load_test.py --drones 50 --duration 60 --rate 30
""",
    )
    parser.add_argument("--host", default="127.0.0.1",
                        help="Server host (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=9000,
                        help="Server port (default: 9000)")
    parser.add_argument("--drones", type=int, default=10,
                        help="Number of simulated drones, 1–1000 (default: 10)")
    parser.add_argument("--duration", type=int, default=30,
                        help="Test duration in seconds (default: 30)")
    parser.add_argument("--rate", type=float, default=20.0,
                        help="Target packets/second per drone (default: 20)")
    args = parser.parse_args()

    if not 1 <= args.drones <= 1000:
        parser.error("--drones must be between 1 and 1000")
    if args.duration < 1:
        parser.error("--duration must be at least 1 second")
    if args.rate < 0.1:
        parser.error("--rate must be at least 0.1 pkt/s")

    return args


def main():
    args = parse_args()
    try:
        asyncio.run(run_load_test(args))
    except KeyboardInterrupt:
        print("\n[!] Interrupted — shutting down.")


if __name__ == "__main__":
    main()
