import os, struct, sys, collections

root = r"V:\Firestorm\Cache\cache"
subs = [d for d in os.listdir(root) if os.path.isdir(os.path.join(root, d))]
subs.sort()
pick = subs[:2]  # two of sixteen subdirs

mesh = 0
other = 0
other_bytes = 0
mesh_bytes = 0
other_sizes = []
tot = 0
for s in pick:
    p = os.path.join(root, s)
    for name in os.listdir(p):
        fp = os.path.join(p, name)
        try:
            sz = os.path.getsize(fp)
            with open(fp, "rb") as f:
                head = f.read(12)
        except OSError:
            continue
        tot += 1
        is_mesh = False
        if len(head) == 12:
            ver, hs, fl = struct.unpack("<III", head)
            if ver == 1 and 0 < hs <= 4096 and sz >= 12 + hs:
                is_mesh = True
        if is_mesh:
            mesh += 1
            mesh_bytes += sz
        else:
            other += 1
            other_bytes += sz
            other_sizes.append(sz)

print("sampled subdirs:", pick)
print("total files    :", tot)
print("mesh           : %d (%.1f%%)  %.1f MB" % (mesh, 100.0*mesh/max(tot,1), mesh_bytes/1048576.0))
print("other          : %d (%.1f%%)  %.1f MB" % (other, 100.0*other/max(tot,1), other_bytes/1048576.0))
if other_sizes:
    other_sizes.sort()
    print("other median   :", other_sizes[len(other_sizes)//2], "max:", other_sizes[-1])
print("extrapolated over 53861 -> mesh %d, other %d" % (53861*mesh//max(tot,1), 53861*other//max(tot,1)))
