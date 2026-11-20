"""Simulated DFC board for testing tools/dfc_update.py over a pty (UART mode,
Linux/macOS only). A Python model of the protocol in src/updater.c, not the
firmware itself: it checks the host side, not the board.

    python3 tools/sim_board.py build/diag/firmware.bin 30   # prints its port, runs 30 s
    py tools/dfc_update.py <that port> --uart --enter flash build/diag/firmware.uf2

Models the updater protocol of src/updater.c (framing, HELLO, BEGIN with
bypass flags, chunked DATA assembly, FINISH checks, trial, CONFIRM, REBOOT)
and the test application's trigger + echo."""
import os, struct, sys, time, zlib, select

SOF = b"\xA5\x5A"
REQ_KEY = 0xDFCC0DEB007ED00D
NO_TRIAL_KEY = 0xDFCBADA55B007ED0
SECTOR = 4096
SLOT = 1012 * 1024

def find_proto(img):
    for i in range(0, len(img) - 31, 4):
        w = struct.unpack_from("<8I", img, i)
        if (w[0], w[1]) == (0x55434644, 0x519C4450) and w[7] == (~(w[0]^w[1]^w[2]^w[3]^w[4]^w[5]^w[6]) & 0xFFFFFFFF):
            return w[2] >> 16, img[i:i+32]
    return -1, None

class Board:
    def __init__(self, fd, running_img):
        self.fd = fd
        self.mode = "app"
        self.slots = {0: running_img, 1: None}
        self.run = 0
        self.trial = False
        self.prev = None
        self.buf = b""
        self.win = b""
        self.log = []
        self.up = None
        self.rx_frames = 0

    def send(self, cmd, payload=b""):
        body = struct.pack("<BH", cmd, len(payload)) + payload
        os.write(self.fd, SOF + body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF))

    def err(self, code, detail=0):
        self.send(0xFF, struct.pack("<BI", code, detail))

    def hello(self):
        p = bytearray(104)
        p[0] = 1; p[1] = 0; p[2] = self.run; p[3] = 3
        struct.pack_into("<III", p, 4, 2048 * 1024, SLOT, SECTOR)
        p[16:16+15] = b"DFC-XIAO-RP2350"
        p[32:32+19] = b"2026-11-20 00:00:00"
        _, blk = find_proto(self.slots[self.run])
        if blk: p[64:96] = blk
        p[96] = 1 if self.trial else 0
        struct.pack_into("<I", p, 100, 64)
        self.send(0x81, bytes(p))

    def reboot_app(self):
        if self.trial:  # unconfirmed: fall back
            self.log.append("fallback to slot %d" % self.prev)
            self.run, self.trial = self.prev, False
        self.mode = "app"; self.buf = b""; self.win = b""

    def handle(self, cmd, p):
        if self.trial and cmd not in (0x01, 0x13, 0x7F):
            return self.err(14)
        if cmd == 0x01:
            return self.hello()
        if cmd == 0x10:
            if len(p) not in (8, 20): return self.err(3)
            size, crc = struct.unpack_from("<II", p)
            flags, key = (struct.unpack_from("<IQ", p, 8) if len(p) == 20 else (0, 0))
            if flags & ~7: return self.err(19, flags)
            if flags & 4 and key != NO_TRIAL_KEY: return self.err(18)
            if size == 0 or size > SLOT: return self.err(11, SLOT)
            self.up = dict(size=size, crc=crc, flags=flags, target=self.run ^ 1, data=bytearray(b"\xff" * size),
                           got=set(), asm=None, fill=0)
            return self.send(0x90, struct.pack("<BxxxIII", self.up["target"], 0x2000 if self.run else 0xFF000, SLOT, flags))
        if cmd == 0x11:
            u = self.up
            if not u: return self.err(5)
            off = struct.unpack_from("<I", p)[0]; d = p[4:]
            if off >= u["size"]: return self.err(6, off)
            idx, within = divmod(off, SECTOR)
            sec_len = min(SECTOR, u["size"] - idx * SECTOR)
            if len(d) > sec_len - within: return self.err(3, sec_len - within)
            if within == 0:
                u["asm"], u["fill"] = idx, 0
            elif u["asm"] != idx or within > u["fill"]:
                return self.err(6, (u["asm"] or 0) * SECTOR + u["fill"])
            u["data"][off:off+len(d)] = d
            u["fill"] = max(u["fill"], within + len(d))
            if u["fill"] == sec_len:
                u["got"].add(idx); u["asm"] = None
            self.rx_frames += 1
            return self.send(0x91, struct.pack("<I", off))
        if cmd == 0x12:
            u = self.up
            if not u: return self.err(5)
            n = (u["size"] + SECTOR - 1) // SECTOR
            for i in range(n):
                if i not in u["got"]: return self.err(10, i * SECTOR)
            crc = zlib.crc32(bytes(u["data"])) & 0xFFFFFFFF
            if crc != u["crc"]: return self.err(9, crc)
            proto, _ = find_proto(bytes(u["data"]))
            if proto < 0 and not u["flags"] & 1: return self.err(12)
            if 0 <= proto < 1 and not u["flags"] & 2: return self.err(13, proto)
            self.send(0x92, struct.pack("<I", crc))
            self.slots[u["target"]] = bytes(u["data"])
            self.prev = self.run; self.run = u["target"]; self.up = None
            if u["flags"] & 4:
                self.log.append("installed permanently in slot %d" % self.run)
                self.trial = False; self.mode = "app"
            else:
                self.log.append("trial boot of slot %d" % self.run)
                self.trial = True; self.mode = "updater"
                self.trial_started = time.monotonic()
            self.buf = b""
            return
        if cmd == 0x13:
            if not self.trial: return self.err(15)
            self.trial = False
            self.log.append("confirmed slot %d" % self.run)
            self.send(0x93)
            self.mode = "app"; self.buf = b""
            return
        return self.err(2)

    def feed(self, data):
        if self.mode == "app":
            for b in data:
                self.win = (self.win + bytes([b]))[-16:]
                if len(self.win) == 16 and self.win[:8] == b"DFC!UPDT":
                    k = struct.unpack("<Q", self.win[8:])[0]
                    if k == REQ_KEY:
                        self.log.append("trigger accepted")
                        self.mode = "updater"; self.buf = b""; self.win = b""
                        return
                    self.log.append("trigger with wrong key rejected")
            os.write(self.fd, data)  # echo application
            return
        self.buf += data
        while True:
            i = self.buf.find(SOF)
            if i < 0: self.buf = self.buf[-1:]; return
            self.buf = self.buf[i:]
            if len(self.buf) < 5: return
            cmd, n = struct.unpack_from("<BH", self.buf, 2)
            if len(self.buf) < 9 + n: return
            body = self.buf[2:5+n]; crc = struct.unpack_from("<I", self.buf, 5+n)[0]
            frame = self.buf[:9+n]; self.buf = self.buf[9+n:]
            if zlib.crc32(body) & 0xFFFFFFFF != crc:
                self.err(1); continue
            if cmd == 0x7F:
                self.send(0x7F | 0x80); self.reboot_app(); return
            self.handle(cmd, body[3:])

    def tick(self):
        if self.mode == "updater" and self.trial and time.monotonic() - self.trial_started > 16.7:
            self.log.append("trial watchdog fired"); self.reboot_app()

def main():
    master, slave = os.openpty()
    print(os.ttyname(slave), flush=True)
    img = open(sys.argv[1], "rb").read()
    b = Board(master, img)
    end = time.monotonic() + float(sys.argv[2])
    while time.monotonic() < end:
        r, _, _ = select.select([master], [], [], 0.05)
        if r:
            b.feed(os.read(master, 65536))
        b.tick()
    print("RESULT", b.log, "running slot", b.run, "trial", b.trial, "data frames", b.rx_frames, flush=True)

main()
