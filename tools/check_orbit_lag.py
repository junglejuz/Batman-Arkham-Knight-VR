import csv, sys, statistics as st
path = sys.argv[1]
rows = list(csv.DictReader(open(path)))
f = lambda r, k: float(r[k])
def wrap(a):
    while a > 180: a -= 360
    while a < -180: a += 360
    return a
dts = [f(rows[i], 't_sec') - f(rows[i-1], 't_sec') for i in range(1, len(rows))]
print('rows', len(rows), 'median dt ms', 1000*st.median(dts))
# per-row base-yaw step, and the rotation actually in the game camera vs what it
# should be (this frame's base + this frame's head)
steps, errs = [], []
for i in range(1, len(rows)):
    a, b = rows[i-1], rows[i]
    if f(b, 'base_yaw_deg') == 0 or f(a, 'base_yaw_deg') == 0: continue
    step = wrap(f(b, 'base_yaw_deg') - f(a, 'base_yaw_deg'))
    live = f(b, 'live_yaw')
    if live == 0: continue
    # what the game drew with at this finalize vs base(this finalize) + head(prev)
    err = wrap(live - (f(b, 'base_yaw_deg') + (f(a, 'final_yaw_deg') - f(a, 'base_yaw_deg'))))
    steps.append(step); errs.append(err)
spin = [(s, e) for s, e in zip(steps, errs) if abs(s) > 1.0]
print('rows with live yaw:', len(steps), ' spinning rows (>1 deg/step):', len(spin))
for s, e in spin[:25]:
    print(f'  base step {s:7.2f}   drawn minus correct {e:7.2f}')
if spin:
    print('median |step| while spinning', st.median(abs(s) for s, _ in spin))
    print('median |err|  while spinning', st.median(abs(e) for _, e in spin))
