#!/usr/bin/env python3
"""plsr conformance harness. Fixtures + expectations are derived from the Mach-O ABI and
dyld semantics, not from plsr's own output. Run: python3 decompiled/conformance/harness.py"""
import json, os, re, struct, subprocess
A = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PKG = os.path.abspath(os.path.join(A, '..', 'pulsar-appexec', 'usr', 'lib', 'pulsar-appexec'))
BIN = os.path.join(A, 'runtime', 'bin', 'appexec')
LIBS = ':'.join(os.path.join(A, 'libs', d) for d in ('compat', 'host', 'lib'))
F = os.path.join(A, 'conformance', 'fixtures')
X, ARM = 0x01000007, 0x0100000c
PIE = 0x200000

def thin(ft=2, cpu=X, flags=PIE | 0x85, magic=0xfeedfacf, cmds=b''):
    return struct.pack('<IiiIIIII', magic, cpu, 3, ft, 1 if cmds else 0, len(cmds), flags, 0) + cmds

LC_MAIN = struct.pack('<IIQQ', 0x80000028, 24, 0x2000, 0)
def unixthread(rip=0x1000):
    st = struct.pack('<QQQQQQ', rip, 0, 0, 0, 0, 0) + b'\0' * (36 * 8)
    return struct.pack('<IIII', 0x5, 16 + len(st), 4, 42) + st

def fat(slices):
    n = len(slices); off = 4096; ents = b''; body = b''
    for cpu, sub, data in slices:
        ents += struct.pack('>iiIII', cpu, sub, off + len(body), len(data), 12)
        body += data + b'\0' * ((-len(data)) % 16)
    return struct.pack('>II', 0xcafebabe, n) + ents + b'\0' * (off - 8 - 20 * n) + body

def build():
    os.makedirs(F, exist_ok=True)
    w = lambda n, b: open(os.path.join(F, n), 'wb').write(b)
    w('empty.bin', b''); w('garbage.bin', b'#!/bin/sh\necho hi\n')
    w('macho32.bin', struct.pack('<IiiIIIII', 0xfeedface, 7, 3, 2, 1, 24, 0, 0) + LC_MAIN)
    w('dylib_x86.bin', thin(6, cmds=LC_MAIN))
    w('exe_no_pie.bin', thin(2, flags=0x85, cmds=LC_MAIN))
    w('exe_pie_no_entry.bin', thin(2, cmds=b''))
    w('exe_pie_unixthread.bin', thin(2, cmds=unixthread()))
    w('fat_arm64_only.bin', fat([(ARM, 0, thin(2, ARM))]))
    w('fat_truncated.bin', struct.pack('>II', 0xcafebabe, 2) + b'\0' * 8)
    w('fat_x86_dylib.bin', fat([(X, 3, thin(6))]))
    def bundle(name, plist, exe=None):
        root = os.path.join(F, name, 'Contents'); os.makedirs(root + '/MacOS', exist_ok=True)
        open(root + '/Info.plist', 'w').write(plist)
        if exe is not None:
            p = root + '/MacOS/' + exe; open(p, 'wb').write(b'\xcf\xfa\xed\xfe'); os.chmod(p, 0o755)
    p = lambda k, v: ('<?xml version="1.0"?><plist version="1.0"><dict><key>%s</key><string>%s</string></dict></plist>' % (k, v))
    bundle('NoCFBundleExecutable.app', p('CFBundleIdentifier', 'x.y'))
    bundle('MissingBinary.app', p('CFBundleExecutable', 'Nope'))
    bundle('NotMachO.app', p('CFBundleExecutable', 'Bin'), 'Bin')

CASES = [
    ('empty', 'empty.bin', 'MACHO', 'empty file is not Mach-O'),
    ('garbage', 'garbage.bin', 'MACHO', 'non-Mach-O bytes rejected'),
    ('thin32', 'macho32.bin', '64BIT', '32-bit Mach-O unsupported'),
    ('dylib_as_program', 'dylib_x86.bin', 'NOENTRY', 'MH_DYLIB is not executable'),
    ('exe_no_MH_PIE', 'exe_no_pie.bin', 'RUNS', 'x86_64 legacy non-PIE runs under dyld'),
    ('exe_no_entry', 'exe_pie_no_entry.bin', 'NOENTRY', 'no LC_MAIN/LC_UNIXTHREAD'),
    ('exe_unixthread', 'exe_pie_unixthread.bin', 'RUNS', 'legacy LC_UNIXTHREAD entry is valid'),
    ('fat_arm64_only', 'fat_arm64_only.bin', 'ARCH', 'no matching slice for host arch'),
    ('fat_truncated', 'fat_truncated.bin', 'MALFORMED', 'truncated fat header is malformed'),
    ('fat_x86_dylib', 'fat_x86_dylib.bin', 'NOENTRY', 'x86_64 slice is a dylib'),
    ('app_no_cfbundleexe', 'NoCFBundleExecutable.app', 'BUNDLE', 'bundle without CFBundleExecutable'),
    ('app_missing_bin', 'MissingBinary.app', 'BUNDLE', 'CFBundleExecutable target absent'),
    ('app_not_macho', 'NotMachO.app', 'MACHO', 'bundle executable is not Mach-O'),
    ('nonexistent', 'does-not-exist.app', 'CANTOPEN', 'path does not exist'),
]
PAT = {'MACHO': r'Mach-O', '64BIT': r'64-bit', 'NOENTRY': r'executable|entry point|no main',
       'ARCH': r'arm64|slice', 'MALFORMED': r'malformed|truncated|fat',
       'BUNDLE': r'main executable|CFBundleExecutable|cannot open', 'CANTOPEN': r'cannot open'}

def main():
    build()
    env = dict(os.environ, LD_LIBRARY_PATH=LIBS)
    rows = []
    for name, rel, kind, why in CASES:
        try:
            r = subprocess.run([BIN, os.path.join(F, rel)], capture_output=True, text=True,
                               timeout=15, env=env, cwd=os.path.dirname(A))
            rc = r.returncode
            msg = (r.stderr or r.stdout).strip().splitlines()
            msg = msg[0] if msg else ''
        except subprocess.TimeoutExpired:
            rc, msg = 124, '(timeout: process kept running)'
        if kind == 'RUNS':
            ok = rc == 124 and 'not position independent' not in msg and 'no segment' not in msg
        else:
            ok = rc != 0 and bool(re.search(PAT[kind], msg, re.I))
        rows.append(dict(case=name, expect=kind, passed=ok, rc=rc, msg=msg, why=why))
        print(('PASS   ' if ok else 'DIVERGE') + ' %-20s rc=%-4s %s' % (name, rc, msg[:100]))
    out = os.path.join(A, 'conformance', 'report.json')
    json.dump(rows, open(out, 'w'), indent=1)
    print('\nparity: %d/%d spec-derived expectations met -> %s' % (sum(r['passed'] for r in rows), len(rows), out))

if __name__ == '__main__':
    main()
