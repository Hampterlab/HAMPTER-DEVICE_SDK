#!/usr/bin/env python3
"""Measure ESP32-C3 SoftAP packet delivery at each official TX power step."""

from __future__ import annotations

import argparse
import json
import re
import socket
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

import serial


POWERS = [8, 20, 28, 34, 44, 52, 60, 68, 74, 78]
SSID = "HAMPTER_TX_DIAG"
AP_IP = "192.168.4.1"
UDP_PORT = 3333


def run(*args: str, timeout: float = 20.0) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        list(args),
        capture_output=True,
        text=True,
        errors="replace",
        timeout=timeout,
        check=False,
    )


def wait_json_line(
    port: serial.Serial,
    event: str,
    timeout: float,
    requested_power: int | None = None,
) -> dict:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        raw = port.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", "replace").strip()
        if not line.startswith("{"):
            continue
        try:
            payload = json.loads(line)
        except json.JSONDecodeError:
            continue
        print(json.dumps({"serial": payload}, ensure_ascii=False), flush=True)
        if payload.get("event") != event:
            continue
        if requested_power is not None and payload.get("requested_quarter_dbm") != requested_power:
            continue
        return payload
    raise TimeoutError(f"serial event {event!r} timed out")


def wlan_connected(interface: str) -> tuple[bool, int | None]:
    result = run("netsh", "wlan", "show", "interfaces")
    text = result.stdout
    ssid_match = re.search(r"^\s*SSID\s*:\s*(.+?)\s*$", text, re.MULTILINE)
    signal_match = re.search(r"^\s*Signal\s*:\s*(\d+)%", text, re.MULTILINE)
    connected = bool(ssid_match and ssid_match.group(1).strip() == SSID)
    signal = int(signal_match.group(1)) if signal_match else None
    return connected, signal


def connect_wlan(interface: str, timeout: float = 15.0) -> int | None:
    run(
        "netsh",
        "wlan",
        "connect",
        f"name={SSID}",
        f"ssid={SSID}",
        f"interface={interface}",
    )
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        connected, signal = wlan_connected(interface)
        if connected:
            return signal
        time.sleep(0.5)
    return None


def udp_echo_test(count: int, timeout: float) -> dict:
    received = 0
    latencies_ms: list[float] = []
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    try:
        for sequence in range(count):
            payload = f"hampter-tx-{sequence:04d}".encode()
            started = time.perf_counter()
            sock.sendto(payload, (AP_IP, UDP_PORT))
            try:
                echoed, peer = sock.recvfrom(512)
            except socket.timeout:
                continue
            if peer[0] == AP_IP and echoed == payload:
                received += 1
                latencies_ms.append((time.perf_counter() - started) * 1000.0)
            time.sleep(0.02)
    finally:
        sock.close()
    return {
        "sent": count,
        "received": received,
        "loss_percent": round((count - received) * 100.0 / count, 2),
        "latency_ms_mean": round(sum(latencies_ms) / len(latencies_ms), 3)
        if latencies_ms
        else None,
        "latency_ms_max": round(max(latencies_ms), 3) if latencies_ms else None,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM4")
    parser.add_argument("--interface", default="Wi-Fi 3")
    parser.add_argument("--packets", type=int, default=50)
    parser.add_argument("--udp-timeout", type=float, default=0.4)
    args = parser.parse_args()

    script_dir = Path(__file__).resolve().parent
    profile = script_dir / "HAMPTER_TX_DIAG.xml"
    results_dir = script_dir.parent / "results"
    results_dir.mkdir(parents=True, exist_ok=True)
    output_path = results_dir / (
        "tx-power-" + datetime.now().strftime("%Y%m%d-%H%M%S") + ".json"
    )

    add_profile = run(
        "netsh",
        "wlan",
        "add",
        "profile",
        f"filename={profile}",
        f"interface={args.interface}",
        "user=current",
    )
    if add_profile.returncode != 0:
        print(add_profile.stdout, file=sys.stderr)
        print(add_profile.stderr, file=sys.stderr)
        return 2

    records: list[dict] = []
    serial_port: serial.Serial | None = None
    try:
        serial_port = serial.Serial(args.port, 115200, timeout=0.25)
        serial_port.dtr = False
        serial_port.rts = False
        wait_json_line(serial_port, "tx_stage_ready", 12.0)

        for raw_power in POWERS:
            run("netsh", "wlan", "disconnect", f"interface={args.interface}")
            serial_port.reset_input_buffer()
            serial_port.write(f"APPLY {raw_power}\n".encode())
            serial_port.flush()
            stage = wait_json_line(
                serial_port, "tx_stage_ready", 10.0, requested_power=raw_power
            )

            signal = connect_wlan(args.interface)
            connected, refreshed_signal = wlan_connected(args.interface)
            if refreshed_signal is not None:
                signal = refreshed_signal
            traffic = (
                udp_echo_test(args.packets, args.udp_timeout)
                if connected
                else {
                    "sent": args.packets,
                    "received": 0,
                    "loss_percent": 100.0,
                    "latency_ms_mean": None,
                    "latency_ms_max": None,
                }
            )

            serial_port.write(b"STATUS\n")
            serial_port.flush()
            status = wait_json_line(
                serial_port,
                "tx_stage_status",
                5.0,
                requested_power=raw_power,
            )
            record = {
                "quarter_dbm": raw_power,
                "dbm": raw_power * 0.25,
                "connected": connected,
                "signal_percent": signal,
                "traffic": traffic,
                "stage": stage,
                "status": status,
            }
            records.append(record)
            print(json.dumps({"measurement": record}, ensure_ascii=False), flush=True)

        serial_port.write(b"APPLY 34\n")
        serial_port.flush()
        wait_json_line(serial_port, "tx_stage_ready", 10.0, requested_power=34)
    finally:
        if serial_port is not None:
            serial_port.close()
        run("netsh", "wlan", "disconnect", f"interface={args.interface}")
        run(
            "netsh",
            "wlan",
            "delete",
            "profile",
            f"name={SSID}",
            f"interface={args.interface}",
        )

    result = {
        "schema_version": 1,
        "created_at": datetime.now(timezone.utc).isoformat(),
        "serial_port": args.port,
        "wlan_interface": args.interface,
        "powers": records,
    }
    output_path.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps({"result_file": str(output_path)}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
