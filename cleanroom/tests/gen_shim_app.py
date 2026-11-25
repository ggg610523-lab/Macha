#!/usr/bin/env python3
"""Generate a Mach-O executable that imports _strtoul and calls it.

The image binds _strtoul through a classic LC_DYLD_INFO bind stream into a
__DATA slot, calls it with ("2a", NULL, 16), and exits 0 only when the result
is 42. Without --shim-lib the loader must fail on the unresolved symbol; with
the clean-room libSystem shim it must run.
"""
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
TEXT, DATA, LINK = 0x100000000, 0x100001000, 0x100002000
OUT = os.path.join(HERE, "shim_strtoul.macho")


def seg(name, vmaddr, vmsize, fileoff, filesize, maxp, initp):
    return struct.pack("<II16sQQQQiiII", 0x19, 72, name, vmaddr, vmsize,
                       fileoff, filesize, maxp, initp, 0, 0)


def lc_dylib(name):
    body = name.encode() + b"\0"
    body += b"\0" * ((8 - len(body) % 8) % 8)
    return struct.pack("<IIIIII", 0x0C, 24 + len(body), 24, 2, 0x10000, 0x10000) + body


def code():
    c = bytearray(b"\x55\x48\x89\xe5")                 # push rbp; mov rbp,rsp
    lea = len(c)
    c += b"\x48\x8d\x3d\x00\x00\x00\x00"               # lea rdi,[rip+num]
    c += b"\x31\xf6"                                   # xor esi,esi
    c += b"\xba\x10\x00\x00\x00"                       # mov edx,16
    call = len(c)
    c += b"\xff\x15\x00\x00\x00\x00"                   # call [rip+got]
    c += b"\x83\xf8\x2a"                               # cmp eax,42
    c += b"\x75\x09"                                   # jne fail
    c += b"\x31\xff"                                   # xor edi,edi
    c += b"\xb8\x3c\x00\x00\x00"                       # mov eax,60
    c += b"\x0f\x05"                                   # syscall
    c += b"\xbf\x01\x00\x00\x00"                       # mov edi,1
    c += b"\xb8\x3c\x00\x00\x00"                       # mov eax,60
    c += b"\x0f\x05"                                   # syscall
    assert len(c) == 50, len(c)
    code_va = 0x200
    struct.pack_into("<i", c, lea + 3, 0x300 - (code_va + lea + 7))
    struct.pack_into("<i", c, call + 2, DATA - (TEXT + code_va + call + 6))
    return bytes(c)


def main():
    bind = b"\x11" b"\x40_strtoul\x00" b"\x51" b"\x71\x00" b"\x90\x00"
    cmds = seg(b"__TEXT", TEXT, 0x1000, 0, 0x1000, 7, 5)
    cmds += seg(b"__DATA", DATA, 0x1000, 0x1000, 0x1000, 3, 3)
    cmds += seg(b"__LINKEDIT", LINK, 0x1000, 0x2000, 0x1000, 1, 1)
    cmds += lc_dylib("/usr/lib/libSystem.B.dylib")
    cmds += struct.pack("<12I", 0x80000022, 48, 0, 0, 0x2000, len(bind),
                        0, 0, 0, 0, 0, 0)
    cmds += struct.pack("<IIQQ", 0x80000028, 24, 0x200, 0)

    b = bytearray(0x3000)
    struct.pack_into("<IiiIIIII", b, 0, 0xFEEDFACF, 0x01000007, 3, 2, 6,
                     len(cmds), 0x200000, 0)
    b[32:32 + len(cmds)] = cmds
    b[0x200:0x200 + 50] = code()
    b[0x300:0x303] = b"2a\0"
    b[0x2000:0x2000 + len(bind)] = bind
    with open(OUT, "wb") as f:
        f.write(bytes(b))
    print("wrote", OUT)


if __name__ == "__main__":
    main()
