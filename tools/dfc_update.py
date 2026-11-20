"""Host side of the DFC firmware updater (Rev 2.25, work in progress: step 3).

The board's updater mode is entered by rebooting into it:
  - diag builds: '!' on the USB console (this script sends it with --enter);
  - the user application: its own magic-key message, which calls
    updater_request(link, key).

In updater mode the board speaks a framed protocol on that link
(src/updater.c):

    0xA5 0x5A | cmd (1) | len (2, LE) | payload | crc32 (4, LE)

with the zlib CRC-32 of cmd, len and payload. Replies set bit 7 of cmd.
After 90 s without a command the board reboots into its firmware.

Commands:
    py tools\\dfc_update.py COM13 --enter                    # '!' then HELLO
    py tools\\dfc_update.py COM13                            # HELLO (already in updater mode)
    py tools\\dfc_update.py COM13 --enter flash firmware.uf2 # update the firmware
    py tools\\dfc_update.py COM13 reboot                     # leave updater mode now

flash writes the image into the slot that is not running, checks it, and the
board then boots it (step 3: no trial boot or confirm yet).
"""

from __future__ import annotations

import argparse
import struct
import sys
import time
import zlib

try:
    import serial
    import serial.tools.list_ports
except ImportError:  # pragma: no cover
    sys.exit("pyserial is required: py -m pip install -r tools/requirements.txt")

SOF = b"\xA5\x5A"
CMD_HELLO = 0x01
CMD_BEGIN = 0x10
CMD_DATA = 0x11
CMD_FINISH = 0x12
CMD_REBOOT = 0x7F
RSP_ERROR = 0xFF
ERRORS = {
    1: "frame CRC error", 2: "unknown command", 3: "bad length", 4: "no A/B partition table",
    5: "no update in progress", 6: "bad offset", 7: "flash operation failed", 8: "read-back verify failed",
    9: "image CRC mismatch", 10: "block missing", 11: "image too big for the slot",
    12: "image has no updater info block", 13: "image's updater protocol is below the board's floor",
}

# Updater protocols this script can drive (src/updater.h UPDATER_PROTOCOL).
# Only images carrying one of these may be installed: anything else could
# leave a unit this toolset can no longer update.
SUPPORTED_PROTOCOLS = {1}
INFO_MAGIC = (0x55434644, 0x519C4450)


class Info:
    """Updater info block (src/updater.c)."""

    def __init__(self, w: tuple[int, ...]):
        self.format = w[2] & 0xFFFF
        self.protocol = w[2] >> 16
        self.version = (w[3] & 0xFFFF, w[3] >> 16, w[4] & 0xFFFF)
        self.floor = w[4] >> 16

    @property
    def version_text(self) -> str:
        return ".".join(map(str, self.version))


def parse_info(raw: bytes, at: int = 0) -> Info | None:
    w = struct.unpack_from("<8I", raw, at)
    check = ~(w[0] ^ w[1] ^ w[2] ^ w[3] ^ w[4] ^ w[5] ^ w[6]) & 0xFFFFFFFF
    if (w[0], w[1]) != INFO_MAGIC or w[7] != check or (w[2] & 0xFFFF) < 1:
        return None
    return Info(w)


def find_info(image: bytes) -> Info | None:
    for i in range(0, len(image) - 31, 4):
        if struct.unpack_from("<I", image, i)[0] == INFO_MAGIC[0]:
            info = parse_info(image, i)
            if info:
                return info
    return None
SECTOR = 4096
FAMILY_RP2350_ARM_S = 0xE48BFF59
UF2_MAGIC = (0x0A324655, 0x9E5D5157, 0x0AB16F30)
UF2_FLAG_NOT_MAIN_FLASH = 0x00000001
UF2_FLAG_FAMILY = 0x00002000
XIP_BASE = 0x10000000


class UpdateError(Exception):
    pass


def load_uf2(path: str, slot_limit: int = 1012 * 1024) -> bytes:
    """The RP2350 Arm image from a .uf2, as a flat binary from XIP_BASE.
    Other families (the RP2350-E10 absolute block) are skipped."""
    data = open(path, "rb").read()
    if len(data) % 512:
        raise UpdateError(f"{path}: not a UF2 file (size is not a multiple of 512)")
    chunks: dict[int, bytes] = {}
    for i in range(0, len(data), 512):
        m0, m1, flags, addr, size, _bno, _nb, fam = struct.unpack_from("<8I", data, i)
        (m_end,) = struct.unpack_from("<I", data, i + 508)
        if (m0, m1, m_end) != UF2_MAGIC:
            raise UpdateError(f"{path}: bad UF2 block at offset {i}")
        if flags & UF2_FLAG_NOT_MAIN_FLASH or not flags & UF2_FLAG_FAMILY or fam != FAMILY_RP2350_ARM_S:
            continue
        if size > 476:
            raise UpdateError(f"{path}: UF2 block payload too large")
        chunks[addr] = data[i + 32:i + 32 + size]
    if not chunks:
        raise UpdateError(f"{path}: no RP2350 Arm (rp2350-arm-s) blocks")
    lo = min(chunks)
    hi = max(a + len(c) for a, c in chunks.items())
    if lo != XIP_BASE:
        raise UpdateError(f"{path}: image starts at 0x{lo:08X}, expected 0x{XIP_BASE:08X}")
    if hi - lo > slot_limit:
        raise UpdateError(f"{path}: image is {hi - lo} bytes, a slot holds {slot_limit}")
    img = bytearray(b"\xff" * (hi - lo))
    for a, c in chunks.items():
        img[a - lo:a - lo + len(c)] = c
    return bytes(img)


def frame(cmd: int, payload: bytes = b"") -> bytes:
    body = struct.pack("<BH", cmd, len(payload)) + payload
    return SOF + body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)


class Link:
    def __init__(self, port: str):
        self.port = port
        self.ser = self._open()
        self.text = b""

    def _open(self) -> serial.Serial:
        s = serial.Serial(self.port, 115200, timeout=0.1)
        s.dtr = True  # the board only talks to an open, DTR-asserted port
        return s

    def close(self) -> None:
        try:
            self.ser.close()
        except serial.SerialException:
            pass

    def send(self, data: bytes) -> None:
        self.ser.write(data)
        self.ser.flush()

    def reply(self, timeout: float = 3.0) -> tuple[int, bytes]:
        """Wait for one valid frame. Plain text before it is printed."""
        buf = b""
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            buf += self.ser.read(4096)
            i = buf.find(SOF)
            if i < 0:
                self._show_text(buf[:-1] if buf.endswith(SOF[:1]) else buf)
                buf = buf[-1:] if buf.endswith(SOF[:1]) else b""
                continue
            self._show_text(buf[:i])
            buf = buf[i:]
            if len(buf) < 5:
                continue
            cmd, n = struct.unpack_from("<BH", buf, 2)
            if len(buf) < 5 + n + 4:
                continue
            body = buf[2:5 + n]
            (crc,) = struct.unpack_from("<I", buf, 5 + n)
            if zlib.crc32(body) & 0xFFFFFFFF != crc:
                buf = buf[2:]  # not a real frame start: keep looking
                continue
            return cmd, buf[5:5 + n]
        raise TimeoutError("no reply from the board (is it in updater mode?)")

    def _show_text(self, data: bytes) -> None:
        self.text += data
        while b"\n" in self.text:
            line, self.text = self.text.split(b"\n", 1)
            line = line.strip()
            if line:
                print("  board:", line.decode("ascii", "replace"))


def wait_for_port(port: str, gone_first: bool, timeout: float = 15.0) -> None:
    """After a reboot the COM port disappears, then comes back."""
    names = lambda: {p.device.upper() for p in serial.tools.list_ports.comports()}  # noqa: E731
    end = time.monotonic() + timeout
    if gone_first:
        while port.upper() in names() and time.monotonic() < end:
            time.sleep(0.1)
    while port.upper() not in names():
        if time.monotonic() > end:
            raise TimeoutError(f"{port} did not come back after the reboot")
        time.sleep(0.2)
    time.sleep(0.5)  # let the driver settle before opening


def show_hello(p: bytes) -> "Info | None":
    proto, link, slot, chip = p[0], p[1], p[2], p[3]
    flash, slot_size, block = struct.unpack_from("<III", p, 4)
    board = p[16:32].split(b"\0", 1)[0].decode("ascii", "replace")
    build = p[32:64].split(b"\0", 1)[0].decode("ascii", "replace")
    slot_name = {0: "A", 1: "B", 0xFF: "none (no partition table)"}.get(slot, f"? ({slot})")
    link_name = {0: "UART0", 1: "UART1", 2: "USB"}.get(link, f"? ({link})")
    print(f"Updater: {board}, protocol {proto}, on {link_name}")
    print(f"  running from slot {slot_name}, chip RP2350 A{chip}")
    print(f"  flash {flash // 1024} KB, slot {slot_size // 1024} KB, block {block} bytes")
    print(f"  build {build}")
    info = parse_info(p, 64) if len(p) >= 96 else None
    if info:
        print(f"  firmware {info.version_text}, updater protocol {info.protocol} (floor {info.floor})")
    return info


def expect(link: Link, want: int, what: str, timeout: float = 5.0) -> bytes:
    cmd, payload = link.reply(timeout)
    if cmd == RSP_ERROR:
        code = payload[0]
        detail = struct.unpack_from("<I", payload, 1)[0] if len(payload) >= 5 else 0
        raise UpdateError(f"{what}: {ERRORS.get(code, f'error {code}')} (detail 0x{detail:X})")
    if cmd != want | 0x80:
        raise UpdateError(f"{what}: unexpected reply 0x{cmd:02X}")
    return payload


def check_versions(new: Info, running: Info | None, allow_older_updater: bool, allow_downgrade: bool) -> None:
    """Refuse images that could strand the unit, or that are older (section
    "Updater info block" in docs/DESIGN.md)."""
    if new.protocol not in SUPPORTED_PROTOCOLS:
        raise UpdateError(f"the image's updater speaks protocol {new.protocol}; this script supports "
                          f"{sorted(SUPPORTED_PROTOCOLS)}. Installing it could leave a unit these tools cannot update.")
    if running is None:
        print("  (the running firmware does not report its versions: skipping the downgrade checks)")
        return
    if new.protocol < running.protocol and not allow_older_updater:
        raise UpdateError(f"the image's updater protocol {new.protocol} is older than the running one "
                          f"({running.protocol}). Use --allow-older-updater if that is intended.")
    if new.version < running.version and not allow_downgrade:
        raise UpdateError(f"the image is firmware {new.version_text}, older than the running "
                          f"{running.version_text}. Use --allow-downgrade if that is intended.")


def flash(link: Link, image: bytes) -> None:
    crc = zlib.crc32(image) & 0xFFFFFFFF
    link.send(frame(CMD_BEGIN, struct.pack("<II", len(image), crc)))
    r = expect(link, CMD_BEGIN, "BEGIN")
    target, (offset, slot_size) = r[0], struct.unpack_from("<II", r, 4)
    print(f"Writing {len(image)} bytes (CRC 0x{crc:08X}) to slot {'AB'[target]} "
          f"at flash offset 0x{offset:X} ({slot_size // 1024} KB slot)")
    blocks = list(range(0, len(image), SECTOR))
    start = time.monotonic()
    for n, off in enumerate(blocks, 1):
        payload = struct.pack("<I", off) + image[off:off + SECTOR]
        for attempt in range(3):
            link.send(frame(CMD_DATA, payload))
            try:
                expect(link, CMD_DATA, f"block at 0x{off:X}")
                break
            except UpdateError as e:
                if "frame CRC" not in str(e) or attempt == 2:
                    raise
        print(f"\r  {n}/{len(blocks)} blocks", end="", flush=True)
    print(f"  ({time.monotonic() - start:.1f} s)")
    link.send(frame(CMD_FINISH))
    try:
        r = expect(link, CMD_FINISH, "FINISH", timeout=10.0)
        print(f"Image verified on the board (CRC 0x{struct.unpack_from('<I', r)[0]:08X}); booting it.")
    except serial.SerialException:
        print("Board rebooted before its FINISH reply was read; check the result below.")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", help="COM port of the board, e.g. COM13")
    ap.add_argument("command", nargs="?", default="hello", choices=("hello", "flash", "reboot"))
    ap.add_argument("file", nargs="?", help="flash: the .uf2 to write (e.g. build\\diag\\firmware.uf2)")
    ap.add_argument("--allow-downgrade", action="store_true",
                    help="flash: allow an older firmware version than the one running")
    ap.add_argument("--allow-older-updater", action="store_true",
                    help="flash: allow an image whose updater protocol is older than the running one")
    ap.add_argument("--enter", action="store_true",
                    help="first send '!' on the diag USB console to reboot into updater mode")
    args = ap.parse_args()
    image = b""
    if args.command == "flash":
        if not args.file:
            ap.error("flash needs a .uf2 file")
        try:
            image = load_uf2(args.file)
            new_info = find_info(image)
            if new_info is None:
                raise UpdateError(f"{args.file} has no updater info block: not a DFC image with an updater "
                                  "(or one from before Rev 2.25). Refusing it.")
            if new_info.protocol not in SUPPORTED_PROTOCOLS:
                check_versions(new_info, None, False, False)  # raises
        except (OSError, UpdateError) as e:
            print("Error:", e)
            return 1
        print(f"Image: firmware {new_info.version_text}, updater protocol {new_info.protocol}, {len(image)} bytes")

    if args.enter:
        link = Link(args.port)
        link.send(b"!")
        time.sleep(0.2)
        link.close()
        print(f"Sent '!': waiting for {args.port} to come back in updater mode...")
        wait_for_port(args.port, gone_first=True)

    link = Link(args.port)
    try:
        if args.command == "flash":
            try:
                link.send(frame(CMD_HELLO))
                running = show_hello(expect(link, CMD_HELLO, "HELLO"))
                check_versions(new_info, running, args.allow_older_updater, args.allow_downgrade)
                flash(link, image)
            except UpdateError as e:
                print("\nError:", e)
                print("The running firmware is untouched. The board leaves updater mode after 90 s idle.")
                return 1
            link.close()
            print(f"Waiting for {args.port} to come back...")
            try:
                wait_for_port(args.port, gone_first=True, timeout=20.0)
                print("Back. Open the terminal: the Firmware line shows the new slot, "
                      "'flash update boot', and the new build time.")
            except TimeoutError as e:
                print(e)
                return 1
        elif args.command == "hello":
            link.send(frame(CMD_HELLO))
            cmd, payload = link.reply()
            if cmd == RSP_ERROR:
                print("Error:", ERRORS.get(payload[0], payload[0]))
                return 1
            if cmd != CMD_HELLO | 0x80:
                print(f"Unexpected reply 0x{cmd:02X}")
                return 1
            show_hello(payload)
        else:
            link.send(frame(CMD_REBOOT))
            try:
                cmd, _ = link.reply()
            except serial.SerialException:
                # The board rebooted and Windows removed the port before the
                # reply was read: the command worked.
                cmd = CMD_REBOOT | 0x80
            print("Rebooting into the firmware." if cmd == CMD_REBOOT | 0x80 else f"Unexpected reply 0x{cmd:02X}")
    finally:
        link.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
