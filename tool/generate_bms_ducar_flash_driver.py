"""Generate the fixed nine-byte UDS FlashDriver identification payload."""

from pathlib import Path


PAYLOAD = b"BMS-DUCAR"
OUTPUT = Path(__file__).with_name("BMS-DUCAR_FlashDriver.bin")


def main() -> None:
    OUTPUT.write_bytes(PAYLOAD)
    result = OUTPUT.read_bytes()
    if result != PAYLOAD or len(result) != 9:
        raise SystemExit("BMS-DUCAR marker verification failed")
    print(f"{OUTPUT}: {len(result)} bytes, {result.hex(' ').upper()}")


if __name__ == "__main__":
    main()
