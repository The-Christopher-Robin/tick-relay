#!/usr/bin/env python3
"""Replays synthetic market-feed frames over TCP to a running tick-relay server.

Each frame is a fixed 64-byte little-endian struct that matches include/feed.h:

    uint32 magic
    uint8  msg_type
    uint8  side
    uint16 reserved0
    uint32 symbol_id
    uint32 qty
    uint64 seq
    uint64 price_cents
    uint64 exchange_ts_ns
    uint64 ingress_tsc   (server fills; we send zero)
    uint64 egress_tsc    (worker fills; we send zero)
    uint32 checksum
    uint32 flags

Usage:
    python3 tools/replay.py --host 127.0.0.1 --port 9001 --messages 500000
"""
import argparse
import random
import socket
import struct
import sys
import time

MAGIC = 0xABCD1234
FEED_FMT = "<IBBH II Q Q Q Q Q II".replace(" ", "")
FEED_SIZE = struct.calcsize(FEED_FMT)
assert FEED_SIZE == 64, f"feed frame must be 64 bytes, got {FEED_SIZE}"

TRADE, QUOTE, HEARTBEAT = 1, 2, 3
SIDE_BID, SIDE_ASK = 0, 1


def xor_checksum(buf: bytes) -> int:
    """Mirror of feed_checksum() in src/feed.c. XOR the first 56 bytes as
    uint32 words, then mix in the Fibonacci-hash constant."""
    acc = 0
    for i in range(0, 56, 4):
        (word,) = struct.unpack_from("<I", buf, i)
        acc ^= word
    acc ^= 0x9E3779B9
    return acc & 0xFFFFFFFF


def build_frame(seq: int, msg_type: int, symbol_id: int,
                price_cents: int, qty: int, side: int,
                flags: int = 0) -> bytes:
    exch_ts_ns = time.monotonic_ns()
    partial = struct.pack(
        FEED_FMT,
        MAGIC,
        msg_type,
        side,
        0,                # reserved0
        symbol_id,
        qty,
        seq,
        price_cents,
        exch_ts_ns,
        0,                # ingress_tsc
        0,                # egress_tsc
        0,                # checksum placeholder
        flags,
    )
    checksum = xor_checksum(partial)
    return struct.pack(
        FEED_FMT,
        MAGIC,
        msg_type,
        side,
        0,
        symbol_id,
        qty,
        seq,
        price_cents,
        exch_ts_ns,
        0,
        0,
        checksum,
        flags,
    )


def iter_frames(count: int, num_symbols: int):
    rng = random.Random(0xC0FFEE)
    for seq in range(count):
        msg_type = rng.choice([TRADE, QUOTE, QUOTE, QUOTE])  # quotes dominate
        symbol_id = rng.randrange(num_symbols)
        price = 10_000 + rng.randint(-500, 500)  # cents
        qty = rng.randint(1, 1000)
        side = rng.choice([SIDE_BID, SIDE_ASK])
        yield build_frame(seq, msg_type, symbol_id, price, qty, side)

    # One trailing heartbeat so the server sees a clean tail frame.
    yield build_frame(count, HEARTBEAT, 0, 0, 0, SIDE_BID)


def send_batched(sock: socket.socket, frames, batch_bytes: int):
    buf = bytearray()
    sent_frames = 0
    for frame in frames:
        buf.extend(frame)
        if len(buf) >= batch_bytes:
            sock.sendall(bytes(buf))
            sent_frames += len(buf) // FEED_SIZE
            buf.clear()
    if buf:
        sock.sendall(bytes(buf))
        sent_frames += len(buf) // FEED_SIZE
    return sent_frames


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9001)
    ap.add_argument("--messages", type=int, default=500_000,
                    help="number of frames to send (default: %(default)s)")
    ap.add_argument("--symbols", type=int, default=1024,
                    help="symbol universe size (default: %(default)s)")
    ap.add_argument("--batch-bytes", type=int, default=64 * 1024,
                    help="send buffer size in bytes (default: 64 KiB)")
    ap.add_argument("--rate", type=float, default=0.0,
                    help="target messages per second; 0 = go as fast as possible")
    args = ap.parse_args(argv)

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        sock.connect((args.host, args.port))
        print(f"replay: connected to {args.host}:{args.port}, "
              f"sending {args.messages} frames")
        t0 = time.monotonic()

        if args.rate > 0:
            period = 1.0 / args.rate
            sent = 0
            start = time.monotonic()
            for frame in iter_frames(args.messages, args.symbols):
                sock.sendall(frame)
                sent += 1
                target = start + sent * period
                now = time.monotonic()
                if target > now:
                    time.sleep(target - now)
        else:
            sent = send_batched(sock, iter_frames(args.messages, args.symbols),
                                args.batch_bytes)

        elapsed = time.monotonic() - t0
        mps = sent / elapsed if elapsed > 0 else 0.0
        print(f"replay: sent {sent} frames in {elapsed:.3f}s "
              f"({mps:,.0f} msg/s)")

    return 0


if __name__ == "__main__":
    sys.exit(main())
