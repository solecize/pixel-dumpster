#!/usr/bin/env python3
"""
pd-ble-bridge — BLE Nordic UART ↔ local TCP NDJSON bridge.

Connects to a pixel-dumpster device advertising Nordic UART Service and
exposes a TCP server. Clients (dumpster-diver, smoke tests) speak the same
newline-terminated JSON protocol used over USB serial.

Usage:
  python3 pd_ble_bridge.py [--name NAME] [--addr ADDR] [--port 9877]

Requires: pip install bleak
"""

from __future__ import annotations

import argparse
import asyncio
import logging
import sys

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    print("pd-ble-bridge requires bleak: pip install bleak", file=sys.stderr)
    sys.exit(1)

# Nordic UART Service UUIDs
NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # host → device (write)
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # device → host (notify)

LOG = logging.getLogger("pd-ble-bridge")


class Bridge:
    def __init__(self, name: str | None, addr: str | None, tcp_port: int):
        self.name = name
        self.addr = addr
        self.tcp_port = tcp_port
        self.client: BleakClient | None = None
        self.tcp_writers: set[asyncio.StreamWriter] = set()
        self._tx_buf = bytearray()

    def _on_notify(self, _handle: int, data: bytearray) -> None:
        self._tx_buf.extend(data)
        while True:
            nl = self._tx_buf.find(b"\n")
            if nl < 0:
                break
            line = bytes(self._tx_buf[: nl + 1])
            del self._tx_buf[: nl + 1]
            dead = []
            for w in self.tcp_writers:
                try:
                    w.write(line)
                    asyncio.get_event_loop().create_task(w.drain())
                except Exception:
                    dead.append(w)
            for w in dead:
                self.tcp_writers.discard(w)

    async def find_device(self):
        if self.addr:
            LOG.info("using address %s", self.addr)
            return self.addr
        LOG.info("scanning for BLE name containing %r …", self.name or "pixel-dumpster")
        needle = (self.name or "pixel-dumpster").lower()
        devices = await BleakScanner.discover(timeout=8.0)
        for d in devices:
            n = (d.name or "").lower()
            if needle in n:
                LOG.info("found %s (%s)", d.name, d.address)
                return d.address
        raise RuntimeError(f"no BLE device matching name {self.name!r}")

    async def handle_tcp(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        peer = writer.get_extra_info("peername")
        LOG.info("TCP client connected %s", peer)
        self.tcp_writers.add(writer)
        try:
            while True:
                line = await reader.readline()
                if not line:
                    break
                if not self.client or not self.client.is_connected:
                    LOG.warning("BLE not connected — drop line")
                    continue
                # Chunk writes to stay under typical ATT MTU.
                data = line
                while data:
                    chunk = data[:180]
                    data = data[180:]
                    await self.client.write_gatt_char(NUS_RX, chunk, response=False)
        finally:
            self.tcp_writers.discard(writer)
            writer.close()
            try:
                await writer.wait_closed()
            except Exception:
                pass
            LOG.info("TCP client disconnected %s", peer)

    async def run(self):
        address = await self.find_device()
        self.client = BleakClient(address)
        await self.client.connect()
        LOG.info("BLE connected to %s", address)
        await self.client.start_notify(NUS_TX, self._on_notify)

        server = await asyncio.start_server(self.handle_tcp, "127.0.0.1", self.tcp_port)
        addrs = ", ".join(str(s.getsockname()) for s in server.sockets or [])
        LOG.info("NDJSON TCP listening on %s", addrs)
        LOG.info("dumpster-diver tip: \"serial\": {\"device\": \"tcp://127.0.0.1:%d\"} or --ble-bridge",
                 self.tcp_port)

        async with server:
            await server.serve_forever()


def main():
    ap = argparse.ArgumentParser(description="pixel-dumpster BLE ↔ TCP NDJSON bridge")
    ap.add_argument("--name", default="pixel-dumpster", help="BLE advertise name substring")
    ap.add_argument("--addr", default=None, help="BLE MAC / UUID address (skip scan)")
    ap.add_argument("--port", type=int, default=9877, help="local TCP port (default 9877)")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
    )
    bridge = Bridge(args.name, args.addr, args.port)
    try:
        asyncio.run(bridge.run())
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
