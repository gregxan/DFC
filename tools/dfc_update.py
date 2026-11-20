"""Host side of the DFC firmware updater (Rev 2.25, work in progress: step 2).

The board's updater mode is entered by rebooting into it:
  - diag builds: '!' on the USB console (this script sends it with --enter);
  - the user application: its own magic-key message, which calls
    updater_request(link, key).

In updater mode the board speaks a framed protocol on that link
(src/updater.c):

    0xA5 0x5A | cmd (1) | len (2, LE) | payload | crc32 (4, LE)

with the zlib CRC-32 of cmd, len and payload. Replies set bit 7 of cmd.
After 90 s without a command the board reboots into its firmware.

Step 2 commands:
    py tools\\dfc_update.py COM13 --enter          # '!' then HELLO
    py tools\\dfc_update.py COM13                  # HELLO (already in updater mode)
    py tools\\dfc_update.py COM13 reboot           # leave updater mode now

Flashing a .uf2 comes in a later step.
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
CMD_REBOOT = 0x7F
RSP_ERROR = 0xFF
ERRORS = {1: "CRC error", 2: "unknown command", 3: "bad length"}


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


def show_hello(p: bytes) -> None:
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


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", help="COM port of the board, e.g. COM13")
    ap.add_argument("command", nargs="?", default="hello", choices=("hello", "reboot"))
    ap.add_argument("--enter", action="store_true",
                    help="first send '!' on the diag USB console to reboot into updater mode")
    args = ap.parse_args()

    if args.enter:
        link = Link(args.port)
        link.send(b"!")
        time.sleep(0.2)
        link.close()
        print(f"Sent '!': waiting for {args.port} to come back in updater mode...")
        wait_for_port(args.port, gone_first=True)

    link = Link(args.port)
    try:
        if args.command == "hello":
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
