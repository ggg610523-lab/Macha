#!/usr/bin/env python3
"""Generate x86_64 Mach-O fixtures with no macOS toolchain: LC_MAIN, LC_UNIXTHREAD, classic
dyld rebase, chained fixups, dylibs with export tries, recursive dependencies, and rpath."""
import os, shutil, struct
HERE = os.path.dirname(os.path.abspath(__file__))
MSG = b'hello from mach-o\n'
DMSG = b'hello from dylib\n'

def seg(name, vmaddr, vmsize, fileoff, filesize, maxp, initp):
    return struct.pack('<II16sQQQQiiII', 0x19, 72, name, vmaddr, vmsize, fileoff, filesize, maxp, initp, 0, 0)

def lc_main(entryoff):
    return struct.pack('<IIQQ', 0x80000028, 24, entryoff, 0)

def uleb(n):
    out = bytearray()
    while True:
        b = n & 0x7f
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)

def trie_one(name, addr):
    """Export trie with a single symbol at `addr`."""
    edge = name.encode() + b'\0'
    term = uleb(0) + uleb(addr)
    child = uleb(len(term)) + term + b'\x00'
    root_len = 1 + 1 + len(edge) + 1
    assert root_len < 128
    return b'\x00\x01' + edge + bytes([root_len]) + child

def lc_dylib(cmd, name):
    b = name.encode() + b'\0'
    b += b'\0' * ((8 - len(b) % 8) % 8)
    return struct.pack('<IIIIII', cmd, 24 + len(b), 24, 2, 0x10000, 0x10000) + b

def lc_rpath(path):
    b = path.encode() + b'\0'
    b += b'\0' * ((8 - len(b) % 8) % 8)
    return struct.pack('<III', 0x8000001c, 12 + len(b), 12) + b

def simple_code(code_off, msg_off):
    """write(1, msg@msg_off, len); exit(0) via rip-relative lea."""
    c  = b'\xb8\x01\x00\x00\x00' + b'\xbf\x01\x00\x00\x00'
    c += b'\x48\x8d\x35' + struct.pack('<i', msg_off - (code_off + 17))
    c += b'\xba' + struct.pack('<I', len(MSG)) + b'\x0f\x05'
    c += b'\xb8\x3c\x00\x00\x00' + b'\x31\xff' + b'\x0f\x05'
    assert len(c) == 33
    return c

def write_simple(path, entry_cmd, code_off=0x100, msg_off=0x121):
    cmds = seg(b'__TEXT', 0x100000000, 0x1000, 0, 0x1000, 7, 5) + entry_cmd
    b = bytearray(0x1000)
    b[0:32] = struct.pack('<IiiIIIII', 0xfeedfacf, 0x01000007, 3, 2, 2, len(cmds), 0x200000, 0)
    b[32:32 + len(cmds)] = cmds
    b[code_off:code_off + 33] = simple_code(code_off, msg_off)
    b[msg_off:msg_off + len(MSG)] = MSG
    open(path, 'wb').write(bytes(b))

def write_unixthread(path):
    st = bytearray(21 * 8)
    struct.pack_into('<Q', st, 16 * 8, 0x100000100)
    write_simple(path, struct.pack('<IIII', 0x5, 16 + len(st), 4, 42) + bytes(st))

def write_noentry(path):
    cmds = seg(b'__TEXT', 0x100000000, 0x1000, 0, 0x1000, 7, 5)
    b = bytearray(0x1000)
    b[0:32] = struct.pack('<IiiIIIII', 0xfeedfacf, 0x01000007, 3, 2, 1, len(cmds), 0x200000, 0)
    b[32:32 + len(cmds)] = cmds
    open(path, 'wb').write(bytes(b))

def write_rebase(path):
    """Classic LC_DYLD_INFO rebase: __DATA[0] holds the message vmaddr."""
    code  = b'\x48\x8d\x05' + struct.pack('<i', 0x100001000 - (0x100000200 + 7))
    code += b'\x48\x8b\x30' + b'\xbf\x01\x00\x00\x00' + b'\xb8\x01\x00\x00\x00'
    code += b'\xba' + struct.pack('<I', len(MSG)) + b'\x0f\x05'
    code += b'\xb8\x3c\x00\x00\x00' + b'\x31\xff' + b'\x0f\x05'
    cmds  = seg(b'__TEXT', 0x100000000, 0x1000, 0, 0x1000, 7, 5)
    cmds += seg(b'__DATA', 0x100001000, 0x1000, 0x1000, 0x1000, 3, 3)
    cmds += seg(b'__LINKEDIT', 0x100002000, 0x1000, 0x2000, 0x1000, 1, 1)
    cmds += struct.pack('<IIIIIIIIIIII', 0x80000022, 48, 0x2000, 5, 0, 0, 0, 0, 0, 0, 0, 0)
    cmds += lc_main(0x200)
    b = bytearray(0x3000)
    b[0:32] = struct.pack('<IiiIIIII', 0xfeedfacf, 0x01000007, 3, 2, 5, len(cmds), 0x200000, 0)
    b[32:32 + len(cmds)] = cmds
    b[0x200:0x200 + len(code)] = code
    b[0x280:0x280 + len(MSG)] = MSG
    struct.pack_into('<Q', b, 0x1000, 0x100000280)
    b[0x2000:0x2005] = b'\x11\x21\x00\x51\x00'
    open(path, 'wb').write(bytes(b))

def call_imports_code():
    """42 bytes: write(1, *(msg ptr), len); _exit(0), both through __DATA function pointers."""
    c  = b'\x4c\x8d\x1d' + struct.pack('<i', 0x100001000 - (0x100000200 + 7))
    c += b'\xba' + struct.pack('<I', len(MSG))
    c += b'\xbf\x01\x00\x00\x00'
    c += b'\x48\x8d\x05' + struct.pack('<i', 0x100001008 - (0x100000200 + 7 + 5 + 5 + 7))
    c += b'\x48\x8b\x30'
    c += b'\x41\xff\x13'
    c += b'\x31\xff'
    c += b'\x4c\x8d\x1d' + struct.pack('<i', 0x100001010 - (0x100000200 + 7 + 5 + 5 + 7 + 3 + 3 + 2 + 7))
    c += b'\x41\xff\x13'
    assert len(c) == 42, len(c)
    return c

def write_bind(path):
    """Classic LC_DYLD_INFO: rebase the msg pointer, bind _write/_exit from ordinal 1."""
    rebase = b'\x11\x21\x08\x51\x00'
    bind  = (b'\x11\x51' b'\x40_write\x00' b'\x71\x00' b'\x90'
             b'\x40_exit\x00'  b'\x71\x10' b'\x90\x00')
    cmds  = seg(b'__TEXT', 0x100000000, 0x1000, 0, 0x1000, 7, 5)
    cmds += seg(b'__DATA', 0x100001000, 0x1000, 0x1000, 0x1000, 3, 3)
    cmds += seg(b'__LINKEDIT', 0x100002000, 0x1000, 0x2000, 0x1000, 1, 1)
    cmds += lc_dylib(0x0c, '/usr/lib/libSystem.B.dylib')
    cmds += struct.pack('<IIIIIIIIIIII', 0x80000022, 48, 0x2000, len(rebase), 0x2100, len(bind), 0, 0, 0, 0, 0, 0)
    cmds += lc_main(0x200)
    b = bytearray(0x3000)
    b[0:32] = struct.pack('<IiiIIIII', 0xfeedfacf, 0x01000007, 3, 2, 6, len(cmds), 0x200000, 0)
    b[32:32 + len(cmds)] = cmds
    b[0x200:0x200 + 42] = call_imports_code()
    b[0x300:0x300 + len(MSG)] = MSG
    struct.pack_into('<Q', b, 0x1008, 0x100000300)
    b[0x2000:0x2000 + len(rebase)] = rebase
    b[0x2100:0x2100 + len(bind)] = bind
    open(path, 'wb').write(bytes(b))

def write_chained(path):
    """Chain of 3 in __DATA: bind _write, rebase msg ptr, bind _exit. Code calls both imports."""
    code = call_imports_code()
    cmds  = seg(b'__TEXT', 0x100000000, 0x1000, 0, 0x1000, 7, 5)
    cmds += seg(b'__DATA', 0x100001000, 0x1000, 0x1000, 0x1000, 3, 3)
    cmds += seg(b'__LINKEDIT', 0x100002000, 0x1000, 0x2000, 0x1000, 1, 1)
    cmds += lc_dylib(0x0c, '/usr/lib/libSystem.B.dylib')
    cmds += struct.pack('<IIII', 0x80000034, 16, 0x2000, 0x5e)
    cmds += lc_main(0x200)

    blob = bytearray(0x60)
    blob[0:28] = struct.pack('<IIIIIII', 0, 0x20, 0x48, 0x50, 2, 1, 0)
    blob[0x20:0x30] = struct.pack('<IIII', 3, 0, 0x10, 0)
    blob[0x30:0x48] = struct.pack('<IHHQIHH', 24, 0x1000, 2, 0x1000, 0, 1, 0)
    blob[0x48:0x50] = struct.pack('<II', 1 | (1 << 9), 1 | (8 << 9))
    blob[0x50:0x5e] = b'\x00_write\x00_exit\x00'
    b = bytearray(0x3000)
    b[0:32] = struct.pack('<IiiIIIII', 0xfeedfacf, 0x01000007, 3, 2, 6, len(cmds), 0x200000, 0)
    b[32:32 + len(cmds)] = cmds
    b[0x200:0x200 + len(code)] = code
    b[0x300:0x300 + len(MSG)] = MSG
    struct.pack_into('<Q', b, 0x1000, (1 << 63) | (2 << 51) | 0)
    struct.pack_into('<Q', b, 0x1008, (2 << 51) | 0x100000300)
    struct.pack_into('<Q', b, 0x1010, (1 << 63) | 1)
    b[0x2000:0x2000 + len(blob)] = blob
    open(path, 'wb').write(bytes(b))

# ---- dylib fixtures -------------------------------------------------------
def write_and_ret(code_off=0x1000, msg_off=0x1100):
    """write(1, msg@msg_off, len); ret -- uses the kernel directly, no imports."""
    c  = b'\xb8\x01\x00\x00\x00' + b'\xbf\x01\x00\x00\x00'
    c += b'\x48\x8d\x35' + struct.pack('<i', msg_off - (code_off + 17))
    c += b'\xba' + struct.pack('<I', len(DMSG)) + b'\x0f\x05' + b'\xc3'
    return c

def write_dylib(path, install, sym, code, deps=(), bind=None):
    """MH_DYLIB with __TEXT (0..0x2000), __DATA (0x2000..0x3000), __LINKEDIT (0x3000..0x4000)."""
    trie = trie_one(sym, 0x1000)
    cmds  = seg(b'__TEXT', 0, 0x2000, 0, 0x2000, 7, 5)
    cmds += seg(b'__DATA', 0x2000, 0x1000, 0x2000, 0x1000, 3, 3)
    cmds += seg(b'__LINKEDIT', 0x3000, 0x1000, 0x3000, 0x1000, 1, 1)
    cmds += lc_dylib(0x0d, install)
    for d in deps:
        cmds += lc_dylib(0x0c, d)
    cmds += struct.pack('<IIII', 0x80000033, 16, 0x3100, len(trie))
    if bind:
        cmds += struct.pack('<IIIIIIIIIIII', 0x80000022, 48, 0, 0, 0x3200, len(bind),
                            0, 0, 0, 0, 0, 0)
    ncmds = 3 + 1 + len(deps) + 1 + (1 if bind else 0)
    b = bytearray(0x4000)
    struct.pack_into('<IiiIIIII', b, 0, 0xfeedfacf, 0x01000007, 3, 6, ncmds, len(cmds), 0x100085, 0)
    b[32:32 + len(cmds)] = cmds
    b[0x1000:0x1000 + len(code)] = code
    b[0x1100:0x1100 + len(DMSG)] = DMSG
    b[0x3100:0x3100 + len(trie)] = trie
    if bind:
        b[0x3200:0x3200 + len(bind)] = bind
    open(path, 'wb').write(bytes(b))

def main_chained(path, deps, sym, rpath=None):
    """Load `sym` from dependency 1 through a __DATA chained bind and call it."""
    name = sym.encode() + b'\0'
    sympool = b'\0' + name
    blob = bytearray(0x50 + len(sympool))
    struct.pack_into('<7I', blob, 0, 0, 0x20, 0x48, 0x50, 1, 1, 0)
    struct.pack_into('<4I', blob, 0x20, 3, 0, 0x10, 0)
    struct.pack_into('<IHHQIHH', blob, 0x30, 24, 0x1000, 6, 0x1000, 0, 1, 0)
    struct.pack_into('<I', blob, 0x48, 1 | (1 << 9))
    blob[0x50:0x50 + len(sympool)] = sympool

    code  = b'\x48\x8b\x05' + struct.pack('<i', 0x100001000 - (0x100000200 + 7))
    code += b'\xff\xd0'                                # call rax
    code += b'\xb8\x3c\x00\x00\x00' + b'\x31\xff' + b'\x0f\x05'

    cmds  = seg(b'__TEXT', 0x100000000, 0x1000, 0, 0x1000, 7, 5)
    cmds += seg(b'__DATA', 0x100001000, 0x1000, 0x1000, 0x1000, 3, 3)
    cmds += seg(b'__LINKEDIT', 0x100002000, 0x1000, 0x2000, 0x1000, 1, 1)
    for d in deps:
        cmds += lc_dylib(0x0c, d)
    if rpath:
        cmds += lc_rpath(rpath)
    cmds += struct.pack('<IIII', 0x80000034, 16, 0x2000, len(blob))
    cmds += lc_main(0x200)

    ncmds = 3 + len(deps) + (1 if rpath else 0) + 2
    b = bytearray(0x3000)
    struct.pack_into('<IiiIIIII', b, 0, 0xfeedfacf, 0x01000007, 3, 2, ncmds, len(cmds), 0x200000, 0)
    b[32:32 + len(cmds)] = cmds
    b[0x200:0x200 + len(code)] = code
    struct.pack_into('<Q', b, 0x1000, (1 << 63) | 0)       # bind import 0, end of chain
    b[0x2000:0x2000 + len(blob)] = blob
    open(path, 'wb').write(bytes(b))

write_simple(os.path.join(HERE, 'hello.macho'), lc_main(0x100))
write_unixthread(os.path.join(HERE, 'hello_unixthread.macho'))
write_noentry(os.path.join(HERE, 'noentry.macho'))
write_rebase(os.path.join(HERE, 'hello_rebase.macho'))
write_chained(os.path.join(HERE, 'hello_chained.macho'))
write_bind(os.path.join(HERE, 'hello_bind.macho'))

# dylib dependency fixtures under a Pulsar-style runtime root
rt = os.path.join(HERE, 'runtime', 'usr', 'lib')
os.makedirs(rt, exist_ok=True)
write_dylib(os.path.join(rt, 'libgreet.dylib'), '/usr/lib/libgreet.dylib',
            '_greet', write_and_ret())
write_dylib(os.path.join(rt, 'libinner.dylib'), '/usr/lib/libinner.dylib',
            '_inner', write_and_ret())
write_dylib(os.path.join(rt, 'libouter.dylib'), '/usr/lib/libouter.dylib',
            '_outer', b'\x48\x8b\x05' + struct.pack('<i', 0x2000 - (0x1000 + 7)) + b'\xff\xe0',
            deps=['/usr/lib/libinner.dylib'],
            bind=b'\x11\x40_inner\x00\x71\x00\x90\x00')

write_dylib(os.path.join(rt, 'libgreet.dylib'), '/usr/lib/libgreet.dylib',
            '_greet', write_and_ret())
main_chained(os.path.join(HERE, 'hello_dylib.macho'), ['/usr/lib/libgreet.dylib'], '_greet')
main_chained(os.path.join(HERE, 'hello_recursive.macho'), ['/usr/lib/libouter.dylib'], '_outer')

# .app with an LC_RPATH into Contents/Frameworks
app = os.path.join(HERE, 'Rpath.app', 'Contents')
os.makedirs(app + '/MacOS', exist_ok=True)
os.makedirs(app + '/Frameworks', exist_ok=True)
open(app + '/Info.plist', 'w').write('<plist><dict><key>CFBundleExecutable</key><string>hello</string></dict></plist>')
main_chained(app + '/MacOS/hello', ['@rpath/libinner.dylib'], '_inner',
             rpath='@executable_path/../Frameworks')
shutil.copy(os.path.join(rt, 'libinner.dylib'), app + '/Frameworks/libinner.dylib')

app = os.path.join(HERE, 'Test.app', 'Contents')
os.makedirs(app + '/MacOS', exist_ok=True)
open(app + '/Info.plist', 'w').write('<plist><dict><key>CFBundleExecutable</key><string>hello</string></dict></plist>')
shutil.copy(os.path.join(HERE, 'hello.macho'), app + '/MacOS/hello')
print('fixtures written to', HERE)
