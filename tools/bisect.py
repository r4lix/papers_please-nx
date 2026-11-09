import subprocess, os, sys
EXE = r'local\host\hosthyb.exe'
SKIP = []
def run(lo, hi, frames=10, timeout=90):
    env = dict(os.environ, LIFT='1', LIFT_RANGE='%d:%d' % (lo, hi), LIFT_SKIP=','.join(map(str, SKIP)))
    try:
        p = subprocess.run([EXE, '--hidden', '--frames', str(frames)], capture_output=True, text=True, timeout=timeout, env=env, errors='replace')
    except subprocess.TimeoutExpired:
        return False, 'timeout'
    ok = p.returncode == 0 and 'done' in p.stdout
    return ok, (p.stdout + p.stderr)
def bisect(lo, hi):
    ok, out = run(lo, hi)
    if ok:
        return None
    while hi - lo > 1:
        mid = (lo + hi) // 2
        okl, _ = run(lo, mid)
        if not okl:
            hi = mid
            continue
        okr, _ = run(mid, hi)
        if not okr:
            lo = mid
            continue
        print('interaction between halves', lo, mid, hi)
        return (lo, hi)
    return (lo, hi)
if __name__ == '__main__':
    lo, hi = int(sys.argv[1]), int(sys.argv[2])
    maxn = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    for _ in range(maxn):
        r = bisect(lo, hi)
        print('culprit range', r, flush=True)
        if r is None:
            break
        SKIP.append(r[0])
    print('skip list', SKIP)
