"""Line I/O with the Brainscape bring-up firmware over USB serial (CDC ACM), stdlib only.

The firmware images (firmware/README.md) speak JSON lines: the host sends one text command
per line, the device answers with one JSON object per line. A capture of that stream (a log
file, or stdin as "-") can stand in for the port, so every tool also checks saved runs.

Ports: Windows "COM5" (opened as \\\\.\\COM5, with read timeouts set through the Win32 API),
Linux "/dev/ttyACM0", macOS "/dev/cu.usbmodem...". Baud rate, parity and DTR do not matter
to a CDC device.
"""
import json
import os
import sys
import time


class Port:
    """A serial port opened as a file, with line reads that time out."""

    def __init__(self, name, timeout=1.0):
        self.name = name
        self._buf = b""
        self._timeout = timeout
        if os.name == "nt":
            path = name if name.startswith("\\\\.\\") else "\\\\.\\" + name
            self._f = open(path, "r+b", buffering=0)
            self._set_windows_timeouts(timeout)
        else:
            import termios
            import tty

            fd = os.open(name, os.O_RDWR | os.O_NOCTTY)
            tty.setraw(fd)
            attrs = termios.tcgetattr(fd)
            attrs[6][termios.VMIN] = 0
            attrs[6][termios.VTIME] = max(1, int(timeout * 10))
            termios.tcsetattr(fd, termios.TCSANOW, attrs)
            self._f = os.fdopen(fd, "r+b", buffering=0)

    def _set_windows_timeouts(self, timeout):
        import ctypes
        import msvcrt
        from ctypes import wintypes

        class COMMTIMEOUTS(ctypes.Structure):
            _fields_ = [("ReadIntervalTimeout", wintypes.DWORD),
                        ("ReadTotalTimeoutMultiplier", wintypes.DWORD),
                        ("ReadTotalTimeoutConstant", wintypes.DWORD),
                        ("WriteTotalTimeoutMultiplier", wintypes.DWORD),
                        ("WriteTotalTimeoutConstant", wintypes.DWORD)]

        maxdword = 0xFFFFFFFF
        # Return as soon as any byte is there, or after `timeout` with none.
        t = COMMTIMEOUTS(maxdword, maxdword, int(timeout * 1000), 0, 2000)
        handle = msvcrt.get_osfhandle(self._f.fileno())
        if not ctypes.windll.kernel32.SetCommTimeouts(wintypes.HANDLE(handle), ctypes.byref(t)):
            raise OSError("SetCommTimeouts failed on %s" % self.name)

    def write_line(self, text):
        self._f.write((text.rstrip("\r\n") + "\n").encode("ascii"))
        self._f.flush()

    def read_line(self, timeout=None):
        """One line without its newline, or None when `timeout` seconds pass without one."""
        deadline = time.monotonic() + (self._timeout if timeout is None else timeout)
        while b"\n" not in self._buf:
            chunk = self._f.read(4096)
            if chunk:
                self._buf += chunk
            elif time.monotonic() >= deadline:
                return None
        line, self._buf = self._buf.split(b"\n", 1)
        return line.decode("utf-8", "replace").rstrip("\r")

    def drain(self, quiet=0.3):
        """Discards whatever arrives until the line is quiet for `quiet` seconds."""
        while self.read_line(timeout=quiet) is not None:
            pass
        self._buf = b""

    def close(self):
        self._f.close()


class LogSource:
    """A captured stream: a file, or stdin for "-"."""

    def __init__(self, path):
        self.name = path
        self._f = sys.stdin if path == "-" else open(path, "r", encoding="utf-8", errors="replace")

    def write_line(self, text):
        pass  # a log cannot be commanded

    def read_line(self, timeout=None):
        line = self._f.readline()
        return line.rstrip("\r\n") if line else None

    def drain(self, quiet=0.3):
        pass

    def close(self):
        if self._f is not sys.stdin:
            self._f.close()


def parse(line):
    """The JSON object on a line, or None for anything else (blank lines, stray text)."""
    line = line.strip()
    if not line.startswith("{"):
        return None
    try:
        obj = json.loads(line)
    except ValueError:
        return None
    return obj if isinstance(obj, dict) else None


def hello(port, attempts=4):
    """Asks the device to identify itself; returns its hello object or None."""
    for _ in range(attempts):
        port.write_line("info")
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            line = port.read_line(timeout=0.5)
            if line is None:
                continue
            obj = parse(line)
            if obj is not None and obj.get("type") == "hello":
                return obj
    return None


def open_source(port=None, log=None, timeout=1.0):
    if (port is None) == (log is None):
        raise SystemExit("give exactly one of --port or --log")
    return Port(port, timeout) if port is not None else LogSource(log)


def describe_hello(h):
    """A few human lines about the device from its hello object."""
    if not h:
        return []
    b = h.get("build", {})
    board = h.get("board", {})
    fp = h.get("fp", {})
    out = ["device: %s image, %s, %s" % (h.get("image", "?"), board.get("version", "?"),
                                         b.get("appType", "?")),
           "  firmware %s%s, libDaisy %s, toolchain %s" % (
               b.get("commit", "?"), " (dirty)" if b.get("dirty") else "", b.get("libDaisy", "?"),
               b.get("toolchain", "?")),
           "  engine archive sha256 %s, sound revision %s" % (
               b.get("engineArchiveSha256", "?"), h.get("soundRevision", "?")),
           "  sysclk %s Hz, boot region %s, FPSCR %s, FPDSCR %s" % (
               board.get("sysclkHz", "?"), board.get("bootRegion", "?"), fp.get("fpscr", "?"),
               fp.get("fpdscr", "?"))]
    return out
