#!/usr/bin/env python3
"""Generate a Mach-O that exercises the Darwin->Linux translations.

It binds _open, _fstat$INODE64, _mmap, _munmap, _close and _exit through a
classic LC_DYLD_INFO bind stream and checks:
  - open() of its own path succeeds,
  - the Darwin 144-byte stat has S_IFREG mode and a nonzero st_size,
  - mmap(PROT_READ, MAP_PRIVATE) returns the file's first byte (0xcf),
  - munmap/close succeed.
Exit status 0 means every check passed.
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from gen_objc_app import Code, seg, lc_dylib, uleb  # noqa: E402

TEXT, DATA, LINK = 0x100000000, 0x100001000, 0x100002000
OUT = os.path.join(HERE, "libc_app.macho")
CODE = 0x400
PATH_OFF = 0x600
GOT = 0x100
STAT = 0x200

SLOTS = {
    "open": 0x00,
    "fstat": 0x08,
    "mmap": 0x10,
    "munmap": 0x18,
    "close": 0x20,
    "exit": 0x28,
}


def jcc(c, opcode):
    """Emit a short conditional jump and patch it to the failure block."""
    c.jumps.append(len(c.b))
    c.b += bytes([opcode, 0])


def build_code():
    c = Code(TEXT + CODE)
    c.raw(b"\x55\x48\x89\xe5")                 # push rbp; mov rbp,rsp
    c.raw(b"\x53\x41\x54\x41\x55")             # push rbx,r12,r13
    c.raw(b"\x48\x83\xec\x08")                 # keep calls 16-byte aligned

    # open(path, O_RDONLY)
    c.lea(0x3D, TEXT + PATH_OFF)
    c.raw(b"\x31\xf6")                         # xor esi,esi
    c.raw(b"\x31\xc0")                         # xor eax,eax (variadic count)
    c.call_got(DATA + GOT + SLOTS["open"])
    c.raw(b"\x85\xc0")
    jcc(c, 0x78)                               # js fail
    c.raw(b"\x41\x89\xc4")                     # mov r12d,eax

    # fstat(fd, &darwin_stat)
    c.raw(b"\x89\xc7")                         # mov edi,eax
    c.lea(0x35, DATA + STAT)
    c.call_got(DATA + GOT + SLOTS["fstat"])
    c.raw(b"\x85\xc0")
    jcc(c, 0x75)                               # jne fail
    c.lea(0x35, DATA + STAT)                   # rsi is caller-saved: reload

    # S_IFMT(st_mode) == S_IFREG
    c.raw(b"\x0f\xb7\x46\x04")                 # movzx eax,word [rsi+4]
    c.raw(b"\x25\x00\xf0\x00\x00")             # and eax,0xf000
    c.raw(b"\x3d\x00\x80\x00\x00")             # cmp eax,0x8000
    jcc(c, 0x75)

    # st_size > 0 (Darwin offset 96)
    c.raw(b"\x48\x83\x7e\x60\x00")             # cmp qword [rsi+96],0
    jcc(c, 0x7E)                               # jle fail

    # mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 0)
    c.raw(b"\x31\xff")                         # xor edi,edi
    c.raw(b"\xbe\x00\x10\x00\x00")             # mov esi,0x1000
    c.raw(b"\xba\x01\x00\x00\x00")             # mov edx,1
    c.raw(b"\xb9\x02\x00\x00\x00")             # mov ecx,2
    c.raw(b"\x45\x89\xe0")                     # mov r8d,r12d
    c.raw(b"\x45\x31\xc9")                     # xor r9d,r9d
    c.call_got(DATA + GOT + SLOTS["mmap"])
    c.raw(b"\x48\x83\xf8\xff")                 # cmp rax,-1
    jcc(c, 0x74)                               # je fail
    c.raw(b"\x49\x89\xc5")                     # mov r13,rax
    c.raw(b"\x80\x38\xcf")                     # cmp byte [rax],0xcf
    jcc(c, 0x75)

    # munmap(map, 4096); close(fd)
    c.raw(b"\x4c\x89\xef")                     # mov rdi,r13
    c.raw(b"\xbe\x00\x10\x00\x00")
    c.call_got(DATA + GOT + SLOTS["munmap"])
    c.raw(b"\x44\x89\xe7")                     # mov edi,r12d
    c.call_got(DATA + GOT + SLOTS["close"])

    c.raw(b"\x31\xff")                         # xor edi,edi
    c.call_got(DATA + GOT + SLOTS["exit"])
    fail = len(c.b)
    c.raw(b"\xbf\x01\x00\x00\x00")             # mov edi,1
    c.call_got(DATA + GOT + SLOTS["exit"])
    return c.patch(fail)


def bind_stream():
    out = bytearray(b"\x11")                   # ordinal 1: libSystem
    for name, off in (("_open", "open"),
                      ("_fstat$INODE64", "fstat"),
                      ("_mmap", "mmap"),
                      ("_munmap", "munmap"),
                      ("_close", "close"),
                      ("_exit", "exit")):
        out += b"\x40" + name.encode() + b"\0"  # set symbol name
        out += b"\x51"                          # set bind type
        out += b"\x71" + uleb(GOT + SLOTS[off])
        out += b"\x90"                          # do bind
    out += b"\x00"
    return bytes(out)


def main():
    path = os.path.abspath(OUT).encode()
    if len(path) >= 0x100:
        raise SystemExit("fixture path too long for the __TEXT slot")
    code = build_code()
    bind = bind_stream()

    cmds = seg(b"__TEXT", TEXT, 0x1000, 0, 0x1000, 7, 5)
    cmds += seg(b"__DATA", DATA, 0x1000, 0x1000, 0x1000, 3, 3)
    cmds += seg(b"__LINKEDIT", LINK, 0x1000, 0x2000, 0x1000, 1, 1)
    cmds += lc_dylib("/usr/lib/libSystem.B.dylib")
    cmds += struct.pack("<12I", 0x80000022, 48, 0, 0, 0x2000, len(bind),
                        0, 0, 0, 0, 0, 0)
    cmds += struct.pack("<IIQQ", 0x80000028, 24, CODE, 0)

    b = bytearray(0x3000)
    struct.pack_into("<IiiIIIII", b, 0, 0xFEEDFACF, 0x01000007, 3, 2, 6,
                     len(cmds), 0x200000, 0)
    b[32:32 + len(cmds)] = cmds
    b[CODE:CODE + len(code)] = code
    b[PATH_OFF:PATH_OFF + len(path) + 1] = path + b"\0"
    b[0x2000:0x2000 + len(bind)] = bind
    with open(OUT, "wb") as f:
        f.write(bytes(b))
    print("wrote", OUT)


if __name__ == "__main__":
    main()
