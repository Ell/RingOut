#!/usr/bin/env python3
"""Opt-in live smoke for Dolphin's hosted traversal protocol.

This deliberately stops at endpoint rendezvous. A full ENet/gameplay test needs
two independently routed machines; two sockets behind one NAT may not support
hairpinning to their own public endpoint.
"""

import argparse
import secrets
import select
import socket
import struct
import sys
import time

PACKET_SIZE = 37
ACK = 0
HELLO_FROM_CLIENT = 2
HELLO_FROM_SERVER = 3
CONNECT_PLEASE = 4
PLEASE_SEND_PACKET = 5
CONNECT_READY = 6
CONNECT_FAILED = 7


def packet(kind: int, request_id: int, payload: bytes = b"") -> bytes:
    data = bytearray(PACKET_SIZE)
    struct.pack_into("<BQ", data, 0, kind, request_id)
    data[9 : 9 + len(payload)] = payload
    return bytes(data)


def ack(sock: socket.socket, server, request_id: int, ok: int = 1) -> None:
    sock.sendto(packet(ACK, request_id, bytes((ok,))), server)


def receive(sock: socket.socket, server, deadline: float):
    while time.monotonic() < deadline:
        timeout = max(0.0, deadline - time.monotonic())
        ready, _, _ = select.select([sock], [], [], timeout)
        if not ready:
            break
        data, source = sock.recvfrom(2048)
        if source != server or len(data) != PACKET_SIZE:
            continue
        kind, request_id = struct.unpack_from("<BQ", data, 0)
        if kind != ACK:
            ack(sock, server, request_id)
        return kind, request_id, data
    raise TimeoutError("hosted traversal service did not complete the exchange")


def hello(sock: socket.socket, server):
    request_id = secrets.randbits(64)
    sock.sendto(packet(HELLO_FROM_CLIENT, request_id, b"\x00"), server)
    deadline = time.monotonic() + 6.0
    saw_ack = False
    while time.monotonic() < deadline:
        kind, response_id, data = receive(sock, server, deadline)
        if kind == ACK and response_id == request_id:
            if data[9] != 1:
                raise RuntimeError("hosted traversal service rejected hello")
            saw_ack = True
        elif kind == HELLO_FROM_SERVER:
            if data[9] != 1:
                raise RuntimeError("hosted traversal protocol version rejected")
            host_id = data[10:18]
            if len(host_id) != 8 or any(c not in b"0123456789abcdef" for c in host_id):
                raise RuntimeError("hosted traversal service returned malformed room code")
            if not saw_ack:
                # UDP may reorder the acknowledgement and reply. The successful
                # HelloFromServer is sufficient; its own ACK was sent above.
                pass
            return host_id
    raise TimeoutError("hosted traversal service did not assign a room code")


def decode_ipv4(data: bytes, offset: int):
    if data[offset] != 0:
        raise RuntimeError("Dolphin traversal returned unsupported IPv6 endpoint")
    address = socket.inet_ntoa(data[offset + 1 : offset + 5])
    port = struct.unpack_from("!H", data, offset + 17)[0]
    if port == 0:
        raise RuntimeError("Dolphin traversal returned an invalid endpoint")
    return address, port


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", default="stun.dolphin-emu.org")
    parser.add_argument("--port", type=int, default=6262)
    args = parser.parse_args()

    resolved = socket.getaddrinfo(
        args.server, args.port, socket.AF_INET, socket.SOCK_DGRAM
    )[0][4]
    host = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    guest = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    host.bind(("0.0.0.0", 0))
    guest.bind(("0.0.0.0", 0))
    try:
        host_id = hello(host, resolved)
        hello(guest, resolved)

        connect_id = secrets.randbits(64)
        guest.sendto(packet(CONNECT_PLEASE, connect_id, host_id), resolved)
        deadline = time.monotonic() + 8.0
        host_endpoint = None
        guest_endpoint = None
        punch_received = False
        while time.monotonic() < deadline and host_endpoint is None:
            ready, _, _ = select.select([host, guest], [], [], 0.25)
            for sock in ready:
                data, source = sock.recvfrom(2048)
                if source != resolved or len(data) != PACKET_SIZE:
                    if sock is guest and data.startswith(b"Hello from Dolphin Netplay"):
                        punch_received = True
                    continue
                kind, request_id = struct.unpack_from("<BQ", data, 0)
                defer_ack = sock is host and kind == PLEASE_SEND_PACKET
                if kind != ACK and not defer_ack:
                    ack(sock, resolved, request_id)
                if sock is host and kind == PLEASE_SEND_PACKET:
                    guest_endpoint = decode_ipv4(data, 9)
                    host.sendto(b"Hello from Dolphin Netplay...", guest_endpoint)
                    # Match TraversalClient: send the endpoint-directed punch
                    # before acknowledging the server instruction. That ACK is
                    # what authorizes ConnectReady.
                    ack(host, resolved, request_id)
                elif sock is guest and kind == CONNECT_READY:
                    original_id = struct.unpack_from("<Q", data, 9)[0]
                    if original_id != connect_id:
                        raise RuntimeError("ConnectReady did not match ConnectPlease")
                    host_endpoint = decode_ipv4(data, 17)
                elif sock is guest and kind == CONNECT_FAILED:
                    reason = data[17]
                    raise RuntimeError(f"hosted traversal ConnectFailed reason={reason}")

        if host_endpoint is None or guest_endpoint is None:
            raise TimeoutError("hosted service did not rendezvous both endpoints")
        punch_deadline = time.monotonic() + 1.0
        while not punch_received and time.monotonic() < punch_deadline:
            ready, _, _ = select.select(
                [guest], [], [], punch_deadline - time.monotonic()
            )
            if not ready:
                break
            data, _ = guest.recvfrom(2048)
            if data.startswith(b"Hello from Dolphin Netplay"):
                punch_received = True
        print(
            "PASS: Dolphin hosted traversal completed Hello, room-code lookup, "
            "PleaseSendPacket, and ConnectReady; "
            f"same-host-punch-received={'yes' if punch_received else 'no'}"
        )
        return 0
    finally:
        host.close()
        guest.close()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, TimeoutError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
