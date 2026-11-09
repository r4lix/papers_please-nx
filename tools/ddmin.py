import subprocess, os, sys
EXE = r'local\host\hosthyb.exe'
def fails(S, frames=8, timeout=60):
    open('local/only.txt', 'w').write(','.join(map(str, sorted(S))))
    env = dict(os.environ, LIFT='1', LIFT_ONLY='local/only.txt')
    try:
        p = subprocess.run([EXE, '--hidden', '--frames', str(frames)], capture_output=True, text=True, timeout=timeout, env=env, errors='replace')
        return not ('done' in p.stdout and p.returncode == 0)
    except subprocess.TimeoutExpired:
        return True
def ddmin(S):
    n = 2
    while len(S) >= 2:
        chunk = max(1, len(S) // n)
        subsets = [S[i:i + chunk] for i in range(0, len(S), chunk)]
        done = False
        for sub in subsets:                      # try a subset
            if fails(sub):
                S = sub; n = 2; done = True; break
        if not done:
            for sub in subsets:                  # try complement
                comp = [x for x in S if x not in sub]
                if comp and fails(comp):
                    S = comp; n = max(n - 1, 2); done = True; break
        if not done:
            if n >= len(S): break
            n = min(len(S), n * 2)
        print('size', len(S), flush=True)
    return S
if __name__ == '__main__':
    lo, hi = int(sys.argv[1]), int(sys.argv[2])
    S = list(range(lo, hi))
    assert fails(S), 'full set does not fail'
    r = ddmin(S)
    print('MINIMAL', r)
