#!/usr/bin/env python3
"""Convert the current DUCAR BIN package into a bare APP1 Motorola S-record."""

from __future__ import annotations

import argparse
import struct
import zlib
from pathlib import Path


HEADER_SIZE = 128
APP1_START_ADDRESS = 0x08010000
APP1_END_ADDRESS = 0x0803F000
SRAM_START_ADDRESS = 0x20000000
SRAM_END_ADDRESS = 0x20024000
S3_DATA_SIZE = 32


def _srec(record_type: int, address: int, address_size: int, data: bytes = b"") -> str:
    address_bytes = address.to_bytes(address_size, "big")
    count = len(address_bytes) + len(data) + 1
    payload = bytes((count,)) + address_bytes + data
    checksum = (~sum(payload)) & 0xFF
    return f"S{record_type}{(payload + bytes((checksum,))).hex().upper()}"


def read_ducar_app_payload(input_path: Path) -> tuple[bytes, int]:
    package = input_path.read_bytes()
    if len(package) < HEADER_SIZE + 8:
        raise ValueError("BIN is too short to contain a DUCAR header and APP vectors")
    if package[:7] != b"DUCAR\xff\xff":
        raise ValueError("BIN does not have the expected DUCAR header marker")

    declared_size = struct.unpack_from("<I", package, 35)[0]
    declared_crc32 = struct.unpack_from("<I", package, 31)[0]
    if declared_size != len(package) - HEADER_SIZE:
        raise ValueError(
            f"DUCAR APP size mismatch: header={declared_size}, actual={len(package) - HEADER_SIZE}"
        )

    payload = package[HEADER_SIZE:]
    if zlib.crc32(payload) & 0xFFFFFFFF != declared_crc32:
        raise ValueError("DUCAR APP payload CRC32 does not match its header")
    if len(payload) > APP1_END_ADDRESS - APP1_START_ADDRESS:
        raise ValueError("APP payload exceeds the APP1 flash range")

    stack_pointer, reset_vector = struct.unpack_from("<II", payload, 0)
    if not SRAM_START_ADDRESS <= stack_pointer <= SRAM_END_ADDRESS:
        raise ValueError(f"APP initial stack pointer is outside SRAM: 0x{stack_pointer:08X}")
    reset_address = reset_vector & ~1
    if not APP1_START_ADDRESS <= reset_address < APP1_START_ADDRESS + len(payload):
        raise ValueError(f"APP reset vector is outside the image: 0x{reset_vector:08X}")
    if reset_vector & 1 == 0:
        raise ValueError(f"APP reset vector is not a Thumb address: 0x{reset_vector:08X}")
    return payload, reset_vector


def generate_app_s19(input_path: Path, output_path: Path) -> None:
    payload, entry_point = read_ducar_app_payload(Path(input_path))
    lines = [_srec(0, 0, 2, b"DUCAR-APP1")]
    record_count = 0
    for offset in range(0, len(payload), S3_DATA_SIZE):
        chunk = payload[offset : offset + S3_DATA_SIZE]
        lines.append(_srec(3, APP1_START_ADDRESS + offset, 4, chunk))
        record_count += 1
    lines.append(_srec(5, record_count, 2))
    lines.append(_srec(7, entry_point, 4))
    output_path = Path(output_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text("\r\n".join(lines) + "\r\n", encoding="ascii", newline="")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    tool_dir = Path(__file__).resolve().parent
    parser.add_argument(
        "input",
        nargs="?",
        type=Path,
        default=tool_dir.parent / "A800000002_7230B1_V01_T12.bin",
        help="DUCAR-wrapped APP BIN (defaults to the current T12 tool BIN)",
    )
    parser.add_argument(
        "output",
        nargs="?",
        type=Path,
        default=tool_dir / "A800000002_7230B1_V01_T12_APP1.s19",
        help="bare APP1 S19 output path",
    )
    args = parser.parse_args()
    generate_app_s19(args.input, args.output)
    payload, _ = read_ducar_app_payload(args.input)
    print(f"Generated {args.output} ({len(payload)} APP bytes at 0x{APP1_START_ADDRESS:08X})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
