#!/usr/bin/env python3
"""sync.py - keeps the vendored copies of the nui library identical to src.

    python sync.py [sync]            write nui_version.h, copy src + .gitattributes + LICENSE + VERSION
                                               into every consumer in consumers.txt (raw bytes)
    python sync.py status            table: consumer, vendored version, tree hash, state; and the
                                               tag built into each DLL in the consumer's libs/
    python sync.py check [--dll P]   status; exit 1 unless every consumer is current (and every
                                               --dll given carries the canonical tree hash)
    python sync.py release <v>       <v> = x.y.z or major | minor | patch: write VERSION, append
                                               RELEASES (version, tree sha256, date), then sync
    python sync.py --self-test       edit a byte of a vendored file in a temp copy; check must fail

Tree hash: sha256 over the sorted relative paths and raw bytes of every vendored file except VERSION and
nui_version.h (that one carries the hash, so it cannot be part of it; status compares it with the text
sync.py would write). Every vendored directory has `.gitattributes` with `* -text`, so the bytes on disk are
the bytes in git whatever core.autocrlf says.

States: current (hash = canonical), stale (1.0.2) (hash of an older release), edited (hash of no release:
the vendored copy was changed, move the change to src), missing.
A built DLL carries "NUI-TAG:<version>:<tree>" (src/version.cpp); status reads it by a byte search.
"""
import contextlib
import datetime
import io
import hashlib
import os
import re
import shutil
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SKIP = {'VERSION', 'nui_version.h'}          # not in the tree hash (top level of the vendored dir / src)
EXTRA = ['.gitattributes', 'LICENSE']          # copied to the root of every vendored dir
TAG_RE = re.compile(rb'NUI-TAG:([0-9A-Za-z.+-]+):([0-9a-f]{64})')


def version_h(version, tree):
    return ('// nui_version.h - written by sync.py; do not edit.\n#pragma once\n'
            '#define NUI_VERSION "%s"\n#define NUI_TREE "%s"\n'
            '#define NUI_TAG "NUI-TAG:" NUI_VERSION ":" NUI_TREE\n' % (version, tree)).encode()


def tree_hash(files):
    h = hashlib.sha256()
    for rel in sorted(files):
        if rel in SKIP:
            continue
        data = files[rel]
        h.update(rel.encode() + b'\0' + str(len(data)).encode() + b'\0' + data)
    return h.hexdigest()


def read_dir(d):
    out = {}
    if d.is_dir():
        for p in sorted(d.rglob('*')):
            if p.is_file():
                out[p.relative_to(d).as_posix()] = p.read_bytes()
    return out


class Lib:
    """nui_dir = the tools/nui folder, root = the Windower root the consumer paths are relative to."""

    def __init__(self, nui_dir, root):
        self.nui, self.root = Path(nui_dir), Path(root)

    # canonical -------------------------------------------------------------------------------------
    def version(self):
        return (self.nui / 'VERSION').read_text().strip()

    def releases(self):
        out = []
        p = self.nui / 'RELEASES'
        if p.exists():
            for line in p.read_text().splitlines():
                f = line.split()
                if len(f) >= 2 and not line.startswith('#'):
                    out.append((f[0], f[1], f[2] if len(f) > 2 else ''))
        return out

    def canonical(self):
        """the vendored file set: {relative path: bytes}, with nui_version.h and VERSION as sync writes them"""
        files = read_dir(self.nui / 'src')
        for e in EXTRA:
            files[e] = (self.nui / e).read_bytes()
        tree = tree_hash(files)
        files['nui_version.h'] = version_h(self.version(), tree)
        files['VERSION'] = ('%s\n%s\n' % (self.version(), tree)).encode()
        return files, tree

    def consumers(self):
        out = []
        for line in (self.nui / 'consumers.txt').read_text().splitlines():
            line = line.strip()
            if line and not line.startswith('#'):
                out.append(line)
        return out

    # commands --------------------------------------------------------------------------------------
    def sync(self):
        files, tree = self.canonical()
        src_h = self.nui / 'src' / 'nui_version.h'
        if not src_h.exists() or src_h.read_bytes() != files['nui_version.h']:
            src_h.write_bytes(files['nui_version.h'])
            print('wrote src/nui_version.h (%s %s)' % (self.version(), tree[:12]))
        for c in self.consumers():
            d = self.root / c
            have = read_dir(d)
            changed = []
            for rel, data in files.items():
                if have.get(rel) != data:
                    t = d / rel
                    t.parent.mkdir(parents=True, exist_ok=True)
                    t.write_bytes(data)
                    changed.append(rel)
            for rel in have:
                if rel not in files:
                    (d / rel).unlink()
                    changed.append(rel + ' (removed)')
            for sub in sorted((p for p in d.rglob('*') if p.is_dir()), reverse=True) if d.is_dir() else []:
                if not any(sub.iterdir()):
                    sub.rmdir()
            print('%-42s %s' % (c, ('%d file(s) changed' % len(changed)) if changed else 'unchanged'))
            for rel in changed:
                print('    ' + rel)
        return 0

    def consumer_state(self, c, files, tree):
        d = self.root / c
        have = read_dir(d)
        if not have or 'VERSION' not in have:
            return 'missing', '-', '-'
        vtxt = have['VERSION'].decode('ascii', 'replace').split()
        vver = vtxt[0] if vtxt else '?'
        vtree = tree_hash(have)
        if vtree == tree:
            # same hash: the files that are not in it must also be what sync writes
            if have.get('nui_version.h') != files['nui_version.h'] or have.get('VERSION') != files['VERSION']:
                return 'edited (VERSION or nui_version.h)', vver, vtree
            return 'current', vver, vtree
        for ver, t, _ in self.releases():
            if t == vtree:
                return 'stale (%s)' % ver, vver, vtree
        return 'edited', vver, vtree

    def dll_tags(self, c):
        """{dll path: (version, tree) | None} for the DLLs in the addon's libs/ (the addon is the parent of plugin/)"""
        libs = (self.root / c).parent.parent / 'libs'
        out = {}
        if libs.is_dir():
            for p in sorted(libs.glob('*.dll')):
                out[p] = self.read_tag(p)
        return out

    @staticmethod
    def read_tag(p):
        try:
            m = TAG_RE.search(Path(p).read_bytes())
        except OSError:
            return None
        return (m.group(1).decode(), m.group(2).decode()) if m else None

    def tag_state(self, tag, tree):
        if tag is None:
            return 'untagged (built before nui tags)'
        if tag[1] == tree:
            return 'current (%s)' % tag[0]
        for ver, t, _ in self.releases():
            if t == tag[1]:
                return 'stale (%s)' % ver
        return 'unknown source (%s %s)' % (tag[0], tag[1][:12])

    def status(self, dlls=(), quiet=False):
        """prints the table; returns the list of problems (empty = check passes)"""
        files, tree = self.canonical()
        problems = []
        out = print if not quiet else (lambda *a: None)
        src_h = self.nui / 'src' / 'nui_version.h'
        out('canonical: tools/nui  version %s  tree %s' % (self.version(), tree[:16]))
        if not src_h.exists() or src_h.read_bytes() != files['nui_version.h']:
            problems.append('src/nui_version.h is not what sync writes (run sync)')
        rel = [r for r in self.releases() if r[0] == self.version()]
        if not rel:
            out('  note: %s has no line in RELEASES yet (python sync.py release %s)' % (self.version(), self.version()))
        elif rel[-1][1] != tree:
            out('  note: src changed since release %s (unreleased changes)' % self.version())
        out('%-42s %-8s %-18s %s' % ('consumer', 'version', 'tree', 'state'))
        for c in self.consumers():
            state, ver, vtree = self.consumer_state(c, files, tree)
            out('%-42s %-8s %-18s %s' % (c, ver, vtree[:16], state))
            if state != 'current':
                problems.append('%s: %s' % (c, state))
            for p, tag in self.dll_tags(c).items():
                out('    %-38s %s' % (p.name, self.tag_state(tag, tree)))
        for p in dlls:
            st = self.tag_state(self.read_tag(p), tree)
            out('dll %s: %s' % (p, st))
            if not st.startswith('current'):
                problems.append('dll %s: %s' % (p, st))
        return problems

    def check(self, dlls=()):
        problems = self.status(dlls)
        if problems:
            print('nui check: FAILED')
            for p in problems:
                print('  ' + p)
            return 1
        print('nui check: ok')
        return 0

    def release(self, what):
        cur = [int(x) for x in self.version().split('.')]
        if what in ('major', 'minor', 'patch'):
            i = {'major': 0, 'minor': 1, 'patch': 2}[what]
            cur[i] += 1
            for j in range(i + 1, 3):
                cur[j] = 0
            new = '.'.join(map(str, cur))
        elif re.fullmatch(r'\d+\.\d+\.\d+', what):
            new = what
        else:
            print('release: give x.y.z, major, minor or patch')
            return 2
        files, tree = self.canonical()
        for ver, t, _ in self.releases():
            if ver == new and t != tree:
                print('release: %s was released with a different tree (%s); pick a new version' % (new, t[:12]))
                return 1
        (self.nui / 'VERSION').write_bytes((new + '\n').encode())
        if not any(ver == new for ver, _, _ in self.releases()):
            with open(self.nui / 'RELEASES', 'a', newline='\n') as f:
                f.write('%s %s %s\n' % (new, tree, datetime.date.today().isoformat()))
        print('released %s (%s)' % (new, tree[:12]))
        return self.sync()


def quiet_check(lib):
    with contextlib.redirect_stdout(io.StringIO()):
        return lib.check()


def self_test():
    """flip a byte of a vendored file in a temp copy: check must fail; pristine must pass; other edits too"""
    lib = Lib(HERE, HERE.parents[1])
    cons = lib.consumers()
    if quiet_check(lib) != 0:
        print('self-test: the real tree must pass check first')
        return 1
    tmp = Path(tempfile.mkdtemp(prefix='nui_selftest_'))
    ok = True
    try:
        shutil.copytree(HERE, tmp / 'tools' / 'nui', ignore=shutil.ignore_patterns('test', '__pycache__'))
        for c in cons:
            shutil.copytree(lib.root / c, tmp / c)
        t = Lib(tmp / 'tools' / 'nui', tmp)

        def expect(name, mutate, want_fail):
            nonlocal ok
            snap = {c: read_dir(tmp / c) for c in cons}
            first = tmp / cons[0]
            mutate(first)
            rc = quiet_check(t)
            good = (rc != 0) == want_fail
            print('self-test %-34s %s' % (name, 'ok' if good else 'WRONG (check returned %d)' % rc))
            ok = ok and good
            for c in cons:                       # restore
                d = tmp / c
                shutil.rmtree(d, ignore_errors=True)
                for rel, data in snap[c].items():
                    (d / rel).parent.mkdir(parents=True, exist_ok=True)
                    (d / rel).write_bytes(data)

        def flip(d):
            p = d / 'ui.cpp'
            b = bytearray(p.read_bytes())
            b[len(b) // 2] ^= 0x01
            p.write_bytes(bytes(b))

        def flip_tag_header(d):
            p = d / 'nui_version.h'
            p.write_bytes(p.read_bytes().replace(b'NUI_VERSION "', b'NUI_VERSION "9', 1))   # any version

        expect('pristine copy', lambda d: None, False)
        expect('one byte flipped in ui.cpp', flip, True)
        expect('nui_version.h edited', flip_tag_header, True)
        expect('a file deleted', lambda d: (d / 'ops.cpp').unlink(), True)
        expect('a file added', lambda d: (d / 'extra.cpp').write_bytes(b'x'), True)
        expect('consumer folder missing', lambda d: shutil.rmtree(d), True)
        # a stale consumer: bump canonical in the temp tree without syncing
        p = tmp / 'tools' / 'nui' / 'src' / 'ui.cpp'
        p.write_bytes(p.read_bytes() + b'\n// changed\n')
        rc = quiet_check(t)
        print('self-test %-34s %s' % ('canonical changed, not synced', 'ok' if rc != 0 else 'WRONG'))
        ok = ok and rc != 0
        with contextlib.redirect_stdout(io.StringIO()):
            t.sync()
        rc = quiet_check(t)
        print('self-test %-34s %s' % ('...then synced', 'ok' if rc == 0 else 'WRONG'))
        ok = ok and rc == 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print('sync self-test: %s' % ('pass' if ok else 'FAILED'))
    return 0 if ok else 1


def main(argv):
    args = argv[1:]
    dlls = []
    while '--dll' in args:
        i = args.index('--dll')
        dlls.append(args[i + 1])
        del args[i:i + 2]
    cmd = args[0] if args else 'sync'
    if cmd == '--self-test':
        return self_test()
    lib = Lib(HERE, HERE.parents[1])
    if cmd == 'sync':
        return lib.sync()
    if cmd == 'status':
        lib.status(dlls)
        return 0
    if cmd == 'check':
        return lib.check(dlls)
    if cmd == 'release':
        return lib.release(args[1] if len(args) > 1 else '')
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
