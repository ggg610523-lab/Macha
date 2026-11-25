#!/usr/bin/env python3
"""Generate a Mach-O that exercises the clean-room Foundation slice.

It binds Foundation class symbols plus _objc_msgSend/_strcmp/_NSLog and checks:
  - +[NSString stringWithUTF8String:] -> -UTF8String
  - +[NSArray arrayWithObjects:count:] -> -count, -objectAtIndex:
  - +[NSNumber numberWithInt:] -> -intValue
  - +[NSDictionary dictionaryWithObjects:forKeys:count:] -> -objectForKey:
  - NSLog("%@ %s %d", array, "plain", 42)
Exit status 0 means every check passed.
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from gen_objc_app import Code, seg, sect, lc_dylib, uleb  # noqa: E402

TEXT, DATA, LINK = 0x100000000, 0x100001000, 0x100002000
OUT = os.path.join(HERE, "foundation_app.macho")
CODE = 0x400

ITEMS, KEYS, VALS = 0x00, 0x20, 0x28
GOT = 0x40
CONST_A, CONST_B, CONST_MISS, CONST_FMT = 0x80, 0x98, 0xB0, 0xC8

STR = {k: 0x600 + i * 0x40 for i, k in enumerate([
    "stringWithUTF8String:", "UTF8String", "hello",
    "arrayWithObjects:count:", "count", "objectAtIndex:", "second",
    "numberWithInt:", "intValue",
    "dictionaryWithObjects:forKeys:count:", "objectForKey:", "missing",
    "fmt", "%@ %s %d\n",
])}


def jne_near(c, out):
    """Emit jne rel32 and remember its displacement field."""
    out.append(len(c.b))
    c.b += b"\x0f\x85\0\0\0\0"


def build_code():
    c = Code(TEXT + CODE)
    near = []
    c.raw(b"\x55\x48\x89\xe5")
    c.raw(b"\x53\x41\x54\x41\x55")             # rbx r12 r13
    c.raw(b"\x48\x83\xec\x08")                 # keep rsp 16-byte aligned at calls

    # NSString stringWithUTF8String:@"hello"
    c.raw(b"\x48\x8b\x3d" + struct.pack("<i", 0))
    got_off = len(c.b) - 4
    c.fix.append((got_off, DATA + GOT + 0x00, got_off + 4))
    c.lea(0x35, TEXT + STR["stringWithUTF8String:"])
    c.lea(0x15, TEXT + STR["hello"])
    c.raw(b"\x31\xc0")
    c.raw(b"\xff\x15" + b"\0" * 4)
    c.fix.append((len(c.b) - 4, DATA + GOT + 0x20, len(c.b)))
    c.raw(b"\x48\x89\xc3")                     # rbx = string

    c.raw(b"\x48\x89\xdf")
    c.lea(0x35, TEXT + STR["UTF8String"])
    c.raw(b"\x31\xc0")
    c.raw(b"\xff\x15" + b"\0" * 4)
    c.fix.append((len(c.b) - 4, DATA + GOT + 0x20, len(c.b)))
    c.raw(b"\x48\x89\xc7")
    c.lea(0x35, TEXT + STR["hello"])
    c.raw(b"\xff\x15" + b"\0" * 4)
    c.fix.append((len(c.b) - 4, DATA + GOT + 0x28, len(c.b)))
    c.raw(b"\x85\xc0")
    jne_near(c, near)

    # NSArray arrayWithObjects:count: with two constants
    c.raw(b"\x48\x8b\x3d" + b"\0" * 4)
    c.fix.append((len(c.b) - 4, DATA + GOT + 0x08, len(c.b)))
    c.lea(0x35, TEXT + STR["arrayWithObjects:count:"])
    c.lea(0x15, DATA + ITEMS)
    c.raw(b"\xb9\x02\x00\x00\x00")             # mov ecx,2
    c.raw(b"\x31\xc0")
    c.raw(b"\xff\x15" + b"\0" * 4)
    c.fix.append((len(c.b) - 4, DATA + GOT + 0x20, len(c.b)))
    c.raw(b"\x49\x89\xc4")                     # r12 = array

    c.raw(b"\x4c\x89\xe7")
    c.lea(0x35, TEXT + STR["count"])
    c.raw(b"\x31\xc0")
    c.raw(b"\xff\x15" + b"\0" * 4)
    c.fix.append((len(c.b) - 4, DATA + GOT + 0x20, len(c.b)))
    c.raw(b"\x83\xf8\x02")
    jne_near(c, near)

    c.raw(b"\x4c\x89\xe7")
    c.lea(0x35, TEXT + STR["objectAtIndex:"])
    c.raw(b"\xba\x01\x00\x00\x00")
    c.raw(b"\x31\xc0")
    c.raw(b"\xff\x15" + b"\0" * 4)
    c.fix.append((len(c.b) - 4, DATA + GOT + 0x20, len(c.b)))
    c.raw(b"\x48\x89\xc7")
    c.lea(0x35, TEXT + STR["UTF8String"])
    c.raw(b"\x31\xc0")
    c.raw(b"\xff\x15" + b"\0" * 4)
    c.fix.append((len(c.b) - 4, DATA + GOT + 0x20, len(c.b)))
    c.raw(b"\x48\x89\xc7")
    c.lea(0x35, TEXT + STR["second"])
    c.raw(b"\xff\x15" + b"\0" * 4)
    c.fix.append((len(c.b) - 4, DATA + GOT + 0x28, len(c.b)))
    c.raw(b"\x85\xc0")
    jne_near(c, near)

    # NSNumber numberWithInt:42 / intValue
    c.raw(b"\x48\x8b\x3d" + b"\0" * 4)
    c.fix.append((len(c.b) - 4, DATA + GOT + 0x10, len(c.b)))
    c.lea(0x35, TEXT + STR["numberWithInt:"])
    c.raw(b"\xba\x2a\x00\x00\x00")
    c.raw(b"\x31\xc0")
    c.raw(b"\xff\x15" + b"\0" * 4)
    c.fix.append((len(c.b) - 4, DATA + GOT + 0x20, len(c.b)))
    c.raw(b"\x48\x89\xc7")
    c.lea(0x35, TEXT + STR["intValue"])
    c.raw(b"\x31\xc0")
    c.raw(b"\xff\x15" + b"\0" * 4)
    c.fix.append((len(c.b) - 4, DATA + GOT + 0x20, len(c.b)))
    c.raw(b"\x83\xf8\x2a")
    jne_near(c, near)

    # NSDictionary with {hello = second}, look up "missing" -> nil
    c.raw(b"\x48\x8b\x3d" + b"\0" * 4)
    c.fix.append((len(c.b) - 4, DATA + GOT + 0x18, len(c.b)))
    c.lea(0x35, TEXT + STR["dictionaryWithObjects:forKeys:count:"])
    c.lea(0x15, DATA + VALS)
    c.lea(0x0d, DATA + KEYS)
    c.raw(b"\x41\xb8\x01\x00\x00\x00")         # mov r8d,1
    c.raw(b"\x31\xc0")
    c.raw(b"\xff\x15" + b"\0" * 4)
    c.fix.append((len(c.b) - 4, DATA + GOT + 0x20, len(c.b)))
    c.raw(b"\x49\x89\xc5")                     # r13 = dict

    c.raw(b"\x4c\x89\xef")
    c.lea(0x35, TEXT + STR["objectForKey:"])
    c.lea(0x15, DATA + CONST_MISS)
    c.raw(b"\x31\xc0")
    c.raw(b"\xff\x15" + b"\0" * 4)
    c.fix.append((len(c.b) - 4, DATA + GOT + 0x20, len(c.b)))
    c.raw(b"\x48\x85\xc0")
    jne_near(c, near)

    # NSLog(cstr format object, array, "plain", 42)
    c.raw(b"\x48\x8d\x3d" + b"\0" * 4)
    c.fix.append((len(c.b) - 4, DATA + CONST_FMT, len(c.b)))
    c.raw(b"\x4c\x89\xe6")                     # rsi = array
    c.lea(0x15, TEXT + STR["second"])
    c.raw(b"\xb9\x2a\x00\x00\x00")             # ecx = 42
    c.raw(b"\x31\xc0")
    c.raw(b"\xff\x15" + b"\0" * 4)
    c.fix.append((len(c.b) - 4, DATA + GOT + 0x30, len(c.b)))

    c.raw(b"\x31\xff")
    c.raw(b"\xb8\x3c\x00\x00\x00\x0f\x05")
    fail = len(c.b)
    c.raw(b"\xbf\x01\x00\x00\x00")
    c.raw(b"\xb8\x3c\x00\x00\x00\x0f\x05")
    for off in near:
        struct.pack_into("<i", c.b, off + 2, fail - (off + 6))
    return c.patch(fail)


def build_data():
    d = bytearray(0x1000)

    def put(off, fmt, *vals):
        struct.pack_into(fmt, d, off, *vals)

    put(ITEMS, "<QQ", DATA + CONST_A, DATA + CONST_B)
    put(KEYS, "<Q", DATA + CONST_A)
    put(VALS, "<Q", DATA + CONST_B)
    put(CONST_A, "<QQI", 0, TEXT + STR["hello"], 5)
    put(CONST_B, "<QQI", 0, TEXT + STR["second"], 6)
    put(CONST_MISS, "<QQI", 0, TEXT + STR["missing"], 7)
    put(CONST_FMT, "<QQI", 0, TEXT + STR["%@ %s %d\n"], 9)
    return bytes(d)


def rebase_stream():
    offsets = [ITEMS, ITEMS + 8, KEYS, VALS,
               CONST_A + 8, CONST_B + 8, CONST_MISS + 8, CONST_FMT + 8]
    out = bytearray(b"\x11\x21")
    out += uleb(0)
    cur = 0
    for off in offsets:
        if off > cur:
            out += b"\x30" + uleb(off - cur)
        out += b"\x51"
        cur = off + 8
    out += b"\x00"
    return bytes(out)


def bind_stream():
    def one(ordinal, name, off):
        return (bytes([0x10 | ordinal]) + b"\x40" + name.encode() + b"\0" +
                b"\x51" + b"\x71" + uleb(off) + b"\x90")

    out = bytearray()
    out += one(1, "_OBJC_CLASS_$_NSConstantString", CONST_A)
    out += one(1, "_OBJC_CLASS_$_NSConstantString", CONST_B)
    out += one(1, "_OBJC_CLASS_$_NSConstantString", CONST_MISS)
    out += one(1, "_OBJC_CLASS_$_NSConstantString", CONST_FMT)
    out += one(1, "_OBJC_CLASS_$_NSString", GOT + 0x00)
    out += one(1, "_OBJC_CLASS_$_NSArray", GOT + 0x08)
    out += one(1, "_OBJC_CLASS_$_NSNumber", GOT + 0x10)
    out += one(1, "_OBJC_CLASS_$_NSDictionary", GOT + 0x18)
    out += one(2, "_objc_msgSend", GOT + 0x20)
    out += one(3, "_strcmp", GOT + 0x28)
    out += one(1, "_NSLog", GOT + 0x30)
    out += b"\x00"
    return bytes(out)


def main():
    code = build_code()
    data = build_data()
    rebase, bind = rebase_stream(), bind_stream()

    cmds = seg(b"__TEXT", TEXT, 0x1000, 0, 0x1000, 7, 5)
    cmds += seg(b"__DATA", DATA, 0x1000, 0x1000, 0x1000, 3, 3)
    cmds += seg(b"__LINKEDIT", LINK, 0x1000, 0x2000, 0x1000, 1, 1)
    cmds += lc_dylib("/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation")
    cmds += lc_dylib("/usr/lib/libobjc.A.dylib")
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
        text = key.encode()
        b[off:off + len(text) + 1] = text + b"\0"
    b[0x1000:0x2000] = data
    b[0x2000:0x2000 + len(rebase)] = rebase
    b[0x2100:0x2100 + len(bind)] = bind
    with open(OUT, "wb") as f:
        f.write(bytes(b))
    print("wrote", OUT)


if __name__ == "__main__":
    main()
