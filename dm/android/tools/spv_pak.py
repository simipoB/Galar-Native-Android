#!/usr/bin/env python3
# Packs the Vulkan shader set (<key>.spv + <key>.vkr, dm/android/tools/vk_shaders.sh) into one file the replay maps (nvn_vk.c, DM_SPV_PAK): 77,000 small files are slow to copy to a phone.
# Layout (little endian): "NXSP" u32 version, u32 count, char chain[16], sorted index, then the data.
# Versions 1/2 have 32-byte entries {key[8], spv_off, spv_len, vkr_off, vkr_len}; versions 3/4 add {gl_spv_off, gl_spv_len}.
# Versions 2/4 store every blob as u32 size + zlib stream (Android ships it inside the APK; nvn_vk.c pak_blob inflates on load).
# Both modes write each shader as they read it, never the whole set at once: holding Sword's 2.3 GB set (and then its 0.7 GB compressed copy) in memory made
#   Android's low-memory killer stop the setup at this step on a 12 GB phone. The raw pack's index comes from the files' sizes, the compressed pack's index is
#   written last; the output is byte for byte what the in-memory version wrote.
# Usage: spv_pak.py <dir b-<chain>> <out.pak>  |  spv_pak.py --compress <in.pak> <out.pak>
import os, struct, sys, zlib
if sys.argv[1] == '--compress':
    src, out = sys.argv[2], sys.argv[3]; f = open(src, 'rb'); h = f.read(28)
    assert h[:4] == b'NXSP' and struct.unpack('<I', h[4:8])[0] in (1, 3), 'not an uncompressed shader pack'
    ver = struct.unpack('<I', h[4:8])[0]; n = struct.unpack('<I', h[8:12])[0]; stride = 44 if ver == 3 else 32
    idx = f.read(stride * n); off = 28 + stride * n; ents = []
    with open(out, 'wb') as g:
        g.write(bytes(off))   # the header and index once every blob's place is known (below)
        for i in range(n):
            e = idx[stride * i:stride * (i + 1)]; so, sl, ro, rl = struct.unpack('<QIQI', e[8:32]); row = [e[:8]]
            pairs = [(so, sl), (ro, rl)]
            if ver == 3:
                go, gl = struct.unpack('<QI', e[32:44]); pairs.append((go, gl))
            for o, l in pairs:
                if not l:
                    row += [0, 0]
                    continue
                f.seek(o); z = struct.pack('<I', l) + zlib.compress(f.read(l), 9); g.write(z); row += [off, len(z)]; off += len(z)
            ents.append(row)
        compressed_ver = 4 if ver == 3 else 2
        g.seek(0); g.write(b'NXSP' + struct.pack('<II', compressed_ver, n) + h[12:28])
        for row in ents:
            if ver == 3: g.write(row[0] + struct.pack('<QIQIQI', *row[1:]))
            else: g.write(row[0] + struct.pack('<QIQI', *row[1:]))
    print(f'{out}: {n} shaders, {off / 1e6:.0f} MB compressed (from {os.path.getsize(src) / 1e6:.0f} MB)'); sys.exit(0)
d, out = sys.argv[1], sys.argv[2]; chain = os.path.basename(d.rstrip('/'))[2:]
keys = sorted(f[:-4] for f in os.listdir(d) if f.endswith('.spv') and not f.endswith('.gl.spv') and os.path.exists(os.path.join(d, f[:-4] + '.vkr')))
has_gl = any(os.path.exists(os.path.join(d, k + '.gl.spv')) for k in keys)
ver = 3 if has_gl else 1; ent = 44 if has_gl else 32; head = 4 + 4 + 4 + 16; off = head + ent * len(keys); idx = []; parts = []
for k in keys:
    s, r = os.path.join(d, k + '.spv'), os.path.join(d, k + '.vkr'); sl, rl = os.path.getsize(s), os.path.getsize(r)
    so, ro = off, off + sl; off += sl + rl; part = [(s, sl), (r, rl)]
    glp = os.path.join(d, k + '.gl.spv')
    if has_gl and os.path.exists(glp):
        gl = os.path.getsize(glp); go = off; off += gl; part.append((glp, gl))
        idx.append((bytes.fromhex(k), so, sl, ro, rl, go, gl))
    elif has_gl: idx.append((bytes.fromhex(k), so, sl, ro, rl, 0, 0))
    else: idx.append((bytes.fromhex(k), so, sl, ro, rl))
    parts.append(part)
with open(out, 'wb') as f:
    f.write(b'NXSP' + struct.pack('<II', ver, len(keys)) + chain.encode().ljust(16, b'\0'))
    for row in idx:
        if has_gl: f.write(row[0] + struct.pack('<QIQIQI', *row[1:]))
        else: f.write(row[0] + struct.pack('<QIQI', *row[1:]))
    for part in parts:
        for p, l in part:
            b = open(p, 'rb').read()
            if len(b) != l: sys.exit(f'[RED] {p} changed size while the pack was written ({l} → {len(b)} bytes)')
            f.write(b)
print(f'{out}: {len(keys)} shaders, {off / 1e6:.0f} MB, chain {chain}, GL depth variants {sum(bool(r[-1]) for r in idx) if has_gl else 0}')
