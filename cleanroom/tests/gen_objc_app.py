#!/usr/bin/env python3
"""Generate a Mach-O Objective-C executable exercising modern metadata.

MyWorker : NSObject has a *relative* method list (12-byte entries, the format
clang emits with LC_DYLD_CHAINED_FIXUPS-era metadata) containing -greet. A
category adds -shout through __objc_catlist. main sends -new, -greet,
-UTF8String, then -shout and -UTF8String, comparing both strings; exit 0 only
when both match. Superclass slots bind to Foundation class/metaclass symbols.
"""
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
TEXT, DATA, LINK = 0x100000000, 0x100001000, 0x100002000
OUT = os.path.join(HERE, "objc_app.macho")
CODE = 0x400   # load commands end below 0x2c0

CLASSLIST, CLASS, META, RO, MRO, MLIST, CONST = 0x00, 0x08, 0x30, 0x58, 0xA0, 0xE8, 0x100
GOT, CATLIST, CAT, CATML, CONST2 = 0x118, 0x138, 0x140, 0x178, 0x198
STR = {k: 0x600 + i * 0x20 for i, k in enumerate(
    ["MyWorker", "new", "greet", "UTF8String", "content", "expected",
     "shout", "shoutmsg", "shoutexp"])}


def seg(name, vmaddr, vmsize, fileoff, filesize, maxp, initp, sects=b""):
    return struct.pack("<II16sQQQQiiII", 0x19, 72 + len(sects), name, vmaddr,
                       vmsize, fileoff, filesize, maxp, initp,
                       len(sects) // 80, 0) + sects


def sect(name, segname, addr, size, offset, align=3, flags=0):
    return struct.pack("<16s16sQQIIIIIIII", name, segname, addr, size, offset,
                       align, 0, 0, flags, 0, 0, 0)


def lc_dylib(name):
    body = name.encode() + b"\0"
    body += b"\0" * ((8 - len(body) % 8) % 8)
    return struct.pack("<IIIIII", 0x0C, 24 + len(body), 24, 2, 0x10000, 0x10000) + body


def uleb(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


class Code:
    def __init__(self, va):
        self.va = va
        self.b = bytearray()
        self.fix = []
        self.jumps = []

    def raw(self, data):
        self.b += data

    def lea(self, modrm, target_va):
        off = len(self.b)
        self.b += b"\x48\x8d" + bytes([modrm]) + b"\0" * 4
        self.fix.append((off + 3, target_va, off + 7))

    def call_got(self, target_va):
        off = len(self.b)
        self.b += b"\xff\x15" + b"\0" * 4
        self.fix.append((off + 2, target_va, off + 6))

    def jne_here(self):
        off = len(self.b)
        self.b += b"\x75\x00"
        self.jumps.append(off)

    def patch(self, fail_off):
        for off in self.jumps:
            self.b[off + 1] = fail_off - (off + 2)
        for at, target, end in self.fix:
            struct.pack_into("<i", self.b, at, target - (self.va + end))
        return bytes(self.b)


def build_code():
    c = Code(TEXT + CODE)
    c.raw(b"\x55\x48\x89\xe5")                 # push rbp; mov rbp,rsp
    c.raw(b"\x53\x41\x54\x41\x55")             # push rbx,r12,r13
    c.raw(b"\x48\x83\xec\x08")                 # sub rsp,8

    c.lea(0x3D, TEXT + STR["MyWorker"])
    c.call_got(DATA + GOT + 0x00)              # objc_getClass
    c.raw(b"\x49\x89\xc4")                     # mov r12,rax

    c.raw(b"\x4c\x89\xe7")
    c.lea(0x35, TEXT + STR["new"])
    c.raw(b"\x31\xc0")
    c.call_got(DATA + GOT + 0x08)              # objc_msgSend
    c.raw(b"\x48\x89\xc3")                     # mov rbx,rax

    for method, expected, got in (("greet", "expected", 0),
                                  ("shout", "shoutexp", 0)):
        c.raw(b"\x48\x89\xdf")                 # mov rdi,rbx
        c.lea(0x35, TEXT + STR[method])
        c.raw(b"\x31\xc0")
        c.call_got(DATA + GOT + 0x08)
        c.raw(b"\x49\x89\xc5")                 # mov r13,rax
        c.raw(b"\x4c\x89\xef")                 # mov rdi,r13
        c.lea(0x35, TEXT + STR["UTF8String"])
        c.raw(b"\x31\xc0")
        c.call_got(DATA + GOT + 0x08)
        c.raw(b"\x48\x89\xc7")                 # mov rdi,rax
        c.lea(0x35, TEXT + STR[expected])
        c.call_got(DATA + GOT + 0x10)          # strcmp
        c.raw(b"\x85\xc0")
        c.jne_here()

    c.raw(b"\x31\xff")                         # xor edi,edi
    c.raw(b"\xb8\x3c\x00\x00\x00\x0f\x05")     # exit(0)
    fail = len(c.b)
    c.raw(b"\xbf\x01\x00\x00\x00")             # mov edi,1
    c.raw(b"\xb8\x3c\x00\x00\x00\x0f\x05")     # exit(1)

    greet = len(c.b)
    c.lea(0x05, DATA + CONST)                  # lea rax,[constant string]
    c.raw(b"\xc3")
    shout = len(c.b)
    c.lea(0x05, DATA + CONST2)
    c.raw(b"\xc3")
    return c.patch(fail), c.va + greet, c.va + shout


def build_data(greet_va, shout_va):
    d = bytearray(0x1000)

    def put(off, fmt, *vals):
        struct.pack_into(fmt, d, off, *vals)

    put(CLASSLIST, "<Q", DATA + CLASS)
    put(CLASS, "<QQQQQ", DATA + META, 0, 0, 0, DATA + RO)
    put(META, "<QQQQQ", DATA + META, 0, 0, 0, DATA + MRO)

    def ro(off, flags, name, methods):
        put(off, "<IIII", flags, 0, 8, 0)
        put(off + 0x10, "<QQQQQQQ", 0, TEXT + name, methods, 0, 0, 0, 0)

    ro(RO, 0, STR["MyWorker"], DATA + MLIST)
    ro(MRO, 1, STR["MyWorker"], 0)

    # Relative method list: entsize 12 | small flag, one entry, offsets from
    # each 32-bit field to its target.
    put(MLIST, "<II", 0x8000000C, 1)
    entry = DATA + MLIST + 8
    put(MLIST + 8, "<iii",
        (TEXT + STR["greet"]) - entry,
        0,
        greet_va - (entry + 8))

    put(CONST, "<QQI", 0, TEXT + STR["content"], 15)
    put(CATLIST, "<Q", DATA + CAT)
    put(CAT, "<QQQQQQQ", TEXT + STR["shout"], DATA + CLASS, DATA + CATML, 0, 0, 0, 0)
    put(CATML, "<II", 24, 1)
    put(CATML + 8, "<QQQ", TEXT + STR["shout"], 0, shout_va)
    put(CONST2, "<QQI", 0, TEXT + STR["shoutmsg"], 15)
    return bytes(d)


def rebase_stream():
    offsets = [CLASSLIST, CLASS + 0x00, CLASS + 0x20, META + 0x00, META + 0x20,
               RO + 0x18, RO + 0x20, MRO + 0x18, CONST + 8,
               CATLIST, CAT + 0x00, CAT + 0x08, CAT + 0x10,
               CATML + 8, CATML + 0x18, CONST2 + 8]
    out = bytearray(b"\x11")
    out += b"\x21" + uleb(0)
    cur = 0
    for off in offsets:
        if off > cur:
            out += b"\x30" + uleb(off - cur)   # ADD_ADDR_ULEB
        out += b"\x51"
        cur = off + 8
    out += b"\x00"
    return bytes(out)


def bind_stream():
    def one(ordinal, name, off):
        return (bytes([0x10 | ordinal]) + b"\x40" + name.encode() + b"\0" +
                b"\x51" + b"\x71" + uleb(off) + b"\x90")

    out = bytearray()
    out += one(2, "_OBJC_CLASS_$_NSObject", CLASS + 0x08)
    out += one(2, "_OBJC_METACLASS_$_NSObject", META + 0x08)
    out += one(2, "_OBJC_CLASS_$_NSConstantString", CONST + 0x00)
    out += one(2, "_OBJC_CLASS_$_NSConstantString", CONST2 + 0x00)
    out += one(1, "_objc_getClass", GOT + 0x00)
    out += one(1, "_objc_msgSend", GOT + 0x08)
    out += one(3, "_strcmp", GOT + 0x10)
    out += b"\x00"
    return bytes(out)


def main():
    code, greet_va, shout_va = build_code()
    data = build_data(greet_va, shout_va)
    rebase, bind = rebase_stream(), bind_stream()

    cmds = seg(b"__TEXT", TEXT, 0x1000, 0, 0x1000, 7, 5)
    cmds += seg(b"__DATA", DATA, 0x1000, 0x1000, 0x1000, 3, 3,
                sect(b"__objc_classlist", b"__DATA", DATA + CLASSLIST, 8, 0x1000) +
                sect(b"__objc_catlist", b"__DATA", DATA + CATLIST, 8, 0x1138))
    cmds += seg(b"__LINKEDIT", LINK, 0x1000, 0x2000, 0x1000, 1, 1)
    cmds += lc_dylib("/usr/lib/libobjc.A.dylib")
    cmds += lc_dylib("/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation")
    cmds += lc_dylib("/usr/lib/libSystem.B.dylib")
    cmds += struct.pack("<12I", 0x80000022, 48, 0x2000, len(rebase),
                        0x2100, len(bind), 0, 0, 0, 0, 0, 0)
    cmds += struct.pack("<IIQQ", 0x80000028, 24, CODE, 0)

    b = bytearray(0x3000)
    struct.pack_into("<IiiIIIII", b, 0, 0xFEEDFACF, 0x01000007, 3, 2, 8,
                     len(cmds), 0x200000, 0)
    b[32:32 + len(cmds)] = cmds
    b[CODE:CODE + len(code)] = code
    for key, off in STR.items():
        text = (b"hello from objc" if key in ("content", "expected")
                else b"category says hello" if key in ("shoutmsg", "shoutexp")
                else key.encode())
        b[off:off + len(text) + 1] = text + b"\0"
    b[0x1000:0x2000] = data
    b[0x2000:0x2000 + len(rebase)] = rebase
    b[0x2100:0x2100 + len(bind)] = bind
    with open(OUT, "wb") as f:
        f.write(bytes(b))
    print("wrote", OUT)


if __name__ == "__main__":
    main()
