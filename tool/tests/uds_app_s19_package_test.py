import json
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
import zlib

sys.dont_write_bytecode = True


ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOL_DIR = ROOT / "tool"
UDS_DIR = TOOL_DIR / "uds_upgrade"
BIN = TOOL_DIR / "A800000002_7230B1_V01_T12.bin"
S19 = UDS_DIR / "rt5_crc32_release.s19"
ZFLASH = UDS_DIR / "ZS_ECU_Flash_20250401.zflash"
APP1_BASE = 0x08010000
HEADER_SIZE = 128


def parse_s19(path):
    records = []
    entry = None
    declared_record_count = None
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
        elif record_type == 0:
            assert payload[3:-1] == b"DUCAR-APP1", "S0 record must identify the APP1 image"
        elif record_type == 5:
            declared_record_count = int.from_bytes(payload[1:3], "big")
        elif record_type == 7:
            entry = int.from_bytes(payload[1:5], "big")
    assert declared_record_count == len(records), "S5 record count must match the S3 data records"
    return records, entry


def ihex_record(record_type, address, data=b""):
    fields = bytes((len(data), (address >> 8) & 0xFF, address & 0xFF, record_type)) + data
    checksum = (-sum(fields)) & 0xFF
    return ":" + (fields + bytes((checksum,))).hex().upper()


def write_ihex(path, address, data):
    upper = address >> 16
    lines = [ihex_record(4, 0, bytes(((upper >> 8) & 0xFF, upper & 0xFF)))]
    low_address = address & 0xFFFF
    for offset in range(0, len(data), 16):
        lines.append(ihex_record(0, low_address + offset, data[offset : offset + 16]))
    lines.append(ihex_record(1, 0))
    path.write_text("\r\n".join(lines) + "\r\n", encoding="ascii")


def verify_tool_generates_all_outputs():
    with tempfile.TemporaryDirectory(prefix="tool-s19-integration-", dir=ROOT / "tool" / "tests") as temporary_directory:
        project = pathlib.Path(temporary_directory) / "project"
        tool_dir = project / "tool"
        app_dir = project / "app"
        boot_dir = project / "boot"
        upgrade_dir = tool_dir / "uds_upgrade"
        for directory in (tool_dir, app_dir, boot_dir, upgrade_dir):
            directory.mkdir(parents=True, exist_ok=True)
        shutil.copy2(TOOL_DIR / "tool.exe", tool_dir / "tool.exe")

        app = bytearray((index * 17 + 5) & 0xFF for index in range(53))
        struct.pack_into("<I", app, 0, 0x20001000)
        struct.pack_into("<I", app, 4, APP1_BASE + 9)
        app = bytes(app)
        (app_dir / "raw.bin").write_bytes(app)
        write_ihex(app_dir / "app.hex", APP1_BASE, app)
        write_ihex(boot_dir / "boot.hex", 0x08000000, bytes(range(16)))

        config = {
            "n32": {
                "software_pn": "A800000002",
                "bat_type": "B1-7230",
                "software_version_sub": "T12",
                "software_version": "V01",
                "app_bin": "app\\raw.bin",
                "app_hex": "app\\app.hex",
                "boot_hex": "boot\\boot.hex",
                "copy_boot_hex": "tool\\boot.hex",
                "output_dir": "tool",
                "app1_s19": "tool\\uds_upgrade\\rt5_crc32_release.s19",
            }
        }
        (tool_dir / "config.json").write_text(json.dumps(config), encoding="utf-8")

        result = subprocess.run(
            [str(tool_dir / "tool.exe"), "-p", "n32"],
            cwd=project,
            capture_output=True,
            text=True,
            errors="replace",
        )
        assert result.returncode == 0, f"tool.exe should generate package outputs: {result.stdout}\n{result.stderr}"

        package_path = tool_dir / "A800000002_B1-7230_V01_T12.bin"
        factory_hex_path = tool_dir / "A800000002_B1-7230_V01_T12.hex"
        s19_path = upgrade_dir / S19.name
        assert package_path.is_file(), "tool.exe must generate the DUCAR BIN package"
        assert factory_hex_path.is_file(), "tool.exe must generate the merged factory HEX"
        assert s19_path.is_file(), "tool.exe must generate CANZPRO APP1 S19 in the same run"

        package = package_path.read_bytes()
        declared_size = struct.unpack_from("<I", package, 35)[0]
        assert declared_size == len(app), "BIN header must store the unpadded APP size"
        assert package[HEADER_SIZE : HEADER_SIZE + len(app)] == app
        assert package[HEADER_SIZE + len(app) :] == b"\xFF" * (128 - len(app))

        records, entry = parse_s19(s19_path)
        recovered = bytearray()
        expected_address = APP1_BASE
        for address, data in records:
            assert address == expected_address, "S19 records must be contiguous from APP1 base"
            recovered.extend(data)
            expected_address += len(data)
        assert recovered == app, "S19 must contain the raw APP without the DUCAR header or BIN padding"
        assert entry == APP1_BASE + 9, "S19 start address must match the APP reset vector"

        factory_hex_path.unlink()
        s19_path.unlink()
        raw_only_result = subprocess.run(
            [str(tool_dir / "tool.exe"), "-p", "n32", "-bin", "app\\raw.bin"],
            cwd=project,
            capture_output=True,
            text=True,
            errors="replace",
        )
        assert raw_only_result.returncode == 0, f"raw-only packaging should succeed: {raw_only_result.stdout}\n{raw_only_result.stderr}"
        assert package_path.is_file(), "raw-only packaging must still generate the DUCAR BIN"
        assert not factory_hex_path.exists(), "raw-only packaging must not generate factory HEX"
        assert not s19_path.exists(), "APP1 S19 must be generated only with BIN and factory HEX"


def main():
    assert S19.name == "rt5_crc32_release.s19", "APP S19 filename must match the customer ZCANPRO flow"
    config = json.loads((TOOL_DIR / "config.json").read_text(encoding="utf-8"))
    assert config["n32"].get("app1_s19", "").replace("\\", "/") == "tool/uds_upgrade/rt5_crc32_release.s19", \
        "N32 profile must configure the customer CANZPRO S19 output"
    verify_tool_generates_all_outputs()
    assert BIN.is_file(), f"missing APP package BIN: {BIN}"
    image = BIN.read_bytes()
    assert image[:7] == b"DUCAR\xff\xff", "input must have the expected DUCAR header"
    declared_size = struct.unpack_from("<I", image, 35)[0]
    declared_crc = struct.unpack_from("<I", image, 31)[0]
    padded_size = (declared_size + HEADER_SIZE - 1) // HEADER_SIZE * HEADER_SIZE
    assert len(image) == HEADER_SIZE + padded_size, "DUCAR payload must be padded to 128-byte blocks"
    payload_end = HEADER_SIZE + declared_size
    payload = image[HEADER_SIZE:payload_end]
    assert image[payload_end:] == b"\xff" * (padded_size - declared_size), "BIN alignment padding must be 0xFF"
    assert zlib.crc32(payload) == declared_crc, "DUCAR APP payload CRC32 mismatch"
    assert struct.unpack_from("<I", payload, 0)[0] >= 0x20000000
    reset_vector = struct.unpack_from("<I", payload, 4)[0]
    assert APP1_BASE <= (reset_vector & ~1) < APP1_BASE + len(payload)

    records, entry = parse_s19(S19)
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
    stale_output_removal = build_script.find(f'del /q "%UDS_DIR%{S19.name}"')
    first_build = build_script.find("-b \"%ROOT%\\boot\\Projects\\MDK\\boot.uvprojx\"")
    assert 0 <= stale_output_removal < first_build, "build must invalidate an old S19 before starting"
    boot_hex_copy = build_script.find("copy /y \"%ROOT%\\boot\\Projects\\MDK\\Objects\\boot.hex\" \"%TOOL_ROOT%boot.hex\"")
    app_build = build_script.find("-b \"%ROOT%\\app\\N32G45x_KEIL\\MDK-ARM\\N32G45x.uvprojx\"")
    assert first_build < boot_hex_copy < app_build, "fresh BOOT HEX must be copied into tool before building APP"
    s19_check = build_script.find(f'if not exist "%UDS_DIR%{S19.name}"')
    assert app_build < s19_check, "build script must check the S19 generated by the APP post-build tool"
    assert "generate_uds_app_s19.py" not in build_script and "PYTHON" not in build_script, \
        "S19 generation must be handled by tool.exe without a separate Python step"
    print("UDS APP1 S19 package test: PASS")


if __name__ == "__main__":
    main()
