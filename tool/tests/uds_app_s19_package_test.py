import importlib.util
import pathlib
import re
import struct
import sys
import tempfile
import xml.etree.ElementTree as ET
import zlib

sys.dont_write_bytecode = True


ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOL_DIR = ROOT / "tool"
UDS_DIR = TOOL_DIR / "uds_upgrade"
GENERATOR = UDS_DIR / "generate_uds_app_s19.py"
BIN = TOOL_DIR / "A800000002_7230B1_V01_T12.bin"
S19 = UDS_DIR / "A800000002_7230B1_V01_T12_APP1.s19"
ZFLASH = UDS_DIR / "ZS_ECU_Flash_20250401.zflash"
APP1_BASE = 0x08010000
HEADER_SIZE = 128


def load_generator():
    spec = importlib.util.spec_from_file_location("generate_uds_app_s19", GENERATOR)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def parse_s19(path):
    records = []
    entry = None
    for raw_line in path.read_text(encoding="ascii").splitlines():
        line = raw_line.strip()
        assert line.startswith("S"), f"invalid S-record line: {line[:12]}"
        record_type = int(line[1])
        payload = bytes.fromhex(line[2:])
        assert payload[0] == len(payload) - 1, "S-record byte count mismatch"
        assert (sum(payload) & 0xFF) == 0xFF, "S-record checksum mismatch"
        if record_type == 3:
            address = int.from_bytes(payload[1:5], "big")
            records.append((address, payload[5:-1]))
        elif record_type == 7:
            entry = int.from_bytes(payload[1:5], "big")
    return records, entry


def main():
    assert BIN.is_file(), f"missing APP package BIN: {BIN}"
    image = BIN.read_bytes()
    assert image[:7] == b"DUCAR\xff\xff", "input must have the expected DUCAR header"
    declared_size = struct.unpack_from("<I", image, 35)[0]
    declared_crc = struct.unpack_from("<I", image, 31)[0]
    assert declared_size == len(image) - HEADER_SIZE, "DUCAR payload size does not match BIN"
    payload = image[HEADER_SIZE:]
    assert zlib.crc32(payload) == declared_crc, "DUCAR APP payload CRC32 mismatch"
    assert struct.unpack_from("<I", payload, 0)[0] >= 0x20000000
    reset_vector = struct.unpack_from("<I", payload, 4)[0]
    assert APP1_BASE <= (reset_vector & ~1) < APP1_BASE + len(payload)

    generator = load_generator()
    with tempfile.TemporaryDirectory(prefix="uds-app1-s19-", dir=ROOT / "tool" / "tests") as temporary_directory:
        generated_s19 = pathlib.Path(temporary_directory) / S19.name
        generator.generate_app_s19(BIN, generated_s19)
        records, entry = parse_s19(generated_s19)
    assert records, "S19 must contain data records"
    recovered = bytearray()
    expected_address = APP1_BASE
    for address, data in records:
        assert address == expected_address, "S19 data records must be contiguous from APP1 base"
        assert data, "empty S3 data record"
        recovered.extend(data)
        expected_address += len(data)
    assert recovered == payload, "S19 payload must exactly match BIN after stripping the 128-byte header"
    assert entry == reset_vector, "S7 start address must match the APP reset vector"

    flow_text = ZFLASH.read_bytes().decode("gb2312")
    flow_text = re.sub(r"^\s*<\?xml[^>]*\?>", "", flow_text, count=1)
    flow = ET.fromstring(flow_text)
    settings = next(item for item in flow.findall("item") if item.attrib.get("type") == "setting")
    assert settings.findtext("phy_addr") == "7a0"
    assert settings.findtext("dst_addr") == "7a8"
    downloads = [item for item in flow.findall("item") if item.attrib.get("type") == "download"]
    download_files = [item.findtext("file") for item in downloads]
    assert [item.replace("\\", "/") for item in download_files] == ["../BMS-DUCAR_FlashDriver.s19", S19.name], "ZCANPRO must download driver placeholder before APP1"
    security_levels = [item.findtext("level") for item in flow.findall("item")
                       if item.attrib.get("type") == "security"]
    assert security_levels == ["1", "9"], "ZCANPRO must use the customer DLL security levels"
    dll_names = [item.findtext("dll") for item in flow.findall("item")
                 if item.attrib.get("type") == "security"]
    assert dll_names == ["SeednKey.dll", "SeednKey.dll"], "ZCANPRO must resolve the customer DLL beside the flow"
    assert (TOOL_DIR / "BMS-DUCAR_FlashDriver.s19").is_file(), "FlashDriver placeholder S19 is missing"

    build_script = (UDS_DIR / "build_uds_upgrade_package.bat").read_text(encoding="utf-8")
    stale_output_removal = build_script.find("del /q \"%UDS_DIR%A800000002_7230B1_V01_T12_APP1.s19\"")
    first_build = build_script.find("-b \"%ROOT%\\boot\\Projects\\MDK\\boot.uvprojx\"")
    assert 0 <= stale_output_removal < first_build, "build must invalidate an old S19 before starting"
    boot_hex_copy = build_script.find("copy /y \"%ROOT%\\boot\\Projects\\MDK\\Objects\\boot.hex\" \"%TOOL_ROOT%boot.hex\"")
    app_build = build_script.find("-b \"%ROOT%\\app\\N32G45x_KEIL\\MDK-ARM\\N32G45x.uvprojx\"")
    assert first_build < boot_hex_copy < app_build, "fresh BOOT HEX must be copied into tool before building APP"
    print("UDS APP1 S19 package test: PASS")


if __name__ == "__main__":
    main()
