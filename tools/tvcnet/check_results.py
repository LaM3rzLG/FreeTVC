# SPDX-License-Identifier: GPL-3.0-or-later
# Checks the memory run_checks.sh saved after each TVC program: 6FF0h log end, 6FF2h failed requests, 7000h log.
import pathlib
import sys

work = pathlib.Path(sys.argv[1])
body = (work / "www" / "hello.txt").read_bytes()
failed = False


def result(name):
    mem = (work / f"{name}.mem").read_bytes()
    end = mem[0] | mem[1] << 8
    return mem, mem[0x10:end - 0x6FF0], mem[2]


def check(name, ok, detail):
    global failed
    print(f"{name}: {'ok' if ok else 'FAILED'}{'' if ok else ' ' + detail}")
    failed |= not ok


mem, log, fails = result("ping")
want = bytes([0, 1, 2, 0, 8]) + b"HELLO\x00\xff\x6a"
check("PING and ECHO", log == want and fails == 0, f"log {log.hex(' ')}, {fails} failed")

mem, log, fails = result("get")
check("GET", log == body and mem[4] == 4 and fails == 0, f"{len(log)} bytes, last status {mem[4]:02X}, {fails} failed")

mem, log, fails = result("tcp")
sent = b"TCP test\x00\x01\x02\xff\xfe\x6a\xa5\r\n"
check("OPEN, SEND, NEXT, CLOSE", log == sent and mem[8:12] == bytes([0, 0, 0, 1]) and fails == 0,
      f"log {log.hex(' ')}, statuses {mem[8:12].hex(' ')}, {fails} failed")

mem, log, fails = result("hayes")
crlf = lambda word: b"\r\n" + word + b"\r\n"
want = (b"ATE0\r" + crlf(b"OK") + crlf(b"2") + crlf(b"OK") + crlf(b"CONNECT") + b"hello, TVCnet\r" + crlf(b"OK")
        + crlf(b"CONNECT") + b"again\r" + crlf(b"OK") + crlf(b"OK") + crlf(b"NO CARRIER") + body + crlf(b"OK")
        + crlf(b"NO CARRIER") + b"0\r4\r")
detail = f"{fails} failed; got:\n{log!r}\nwanted:\n{want!r}"
check("Hayes session", log == want and fails == 0, detail)
sys.exit(1 if failed else 0)
