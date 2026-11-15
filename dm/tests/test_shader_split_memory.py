#!/usr/bin/env python3
# shader_split.py's scan with little memory: pmap the way it runs on Android (no named semaphores, so no process pool: forked workers that write their results to
# files) gives the results one at a time in the items' order, a worker the low-memory killer stops has the rest of its share run again, and no result file is left
# behind; file_holds finds a mark in a big file read in pieces, also across two pieces. Run: python3 -I dm/tests/test_shader_split_memory.py
import os, signal, sys, tempfile
os.environ.setdefault('DM_GAME', '01008DB008C2C000')
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../gfx/tools'))
sys.modules['multiprocessing.synchronize'] = None   # import fails as on Android's Python
import shader_split

MARK = None
def work(x):
    if x == 13 and not os.path.exists(MARK):   # the first time item 13 comes up, its worker is killed as Android's low-memory killer would
        open(MARK, 'w').close(); os.kill(os.getpid(), signal.SIGKILL)
    return (f'f{x}', {f'k{x}': ('where', 'vs', b'c' * 100, b'k' * x)}, set(), {})

def main():
    global MARK
    with tempfile.TemporaryDirectory() as t:
        tempfile.tempdir = t; MARK = os.path.join(t, 'killed')
        got = shader_split.pmap(4, work, list(range(50)), (None, {}, {}))
        assert not isinstance(got, list), 'pmap gives its results one at a time'
        got = list(got)
        assert [g[0] for g in got] == [f'f{x}' for x in range(50)], [g[0] for g in got]
        assert all(g[1][f'k{x}'][3] == b'k' * x for x, g in enumerate(got))
        assert os.path.exists(MARK), 'the worker with item 13 was killed'
        assert not [f for f in os.listdir(t) if f.endswith('.pk')], os.listdir(t)
        assert list(shader_split.pmap(3, work, [], (None, {}, {}))) == []
        assert list(shader_split.pmap(8, work, [1, 2], (None, {}, {}))) == [work(1), work(2)]
        assert not [f for f in os.listdir(t) if f.endswith('.pk')], os.listdir(t)
        tempfile.tempdir = None
    print('pmap (forked workers): results in order, a killed worker\'s share finished, no result files left')
    with tempfile.TemporaryDirectory() as t:
        p = os.path.join(t, 'big.bin'); piece = 1 << 24
        for at, mark, want in ((piece - 3, b'BNSH\0\0\0\0', True), (piece - 1, b'BAHS', True), (2 * piece + 5, b'BAHS', True), (None, None, False)):
            d = bytearray(2 * piece + 9)
            if at is not None: d[at:at + len(mark)] = mark
            open(p, 'wb').write(d)
            assert shader_split.file_holds(p, [b'BNSH\0\0\0\0', b'BAHS']) == want, (at, mark)
    print('file_holds: marks found inside a piece, across two pieces and at the end; none in a file without them')

if __name__ == '__main__':
    main()
