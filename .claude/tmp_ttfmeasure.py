import struct, sys

path = r"D:\soapstorm\build-vc180-64\packages\fonts\InterVariableFont.ttf"
data = open(path, 'rb').read()

def u16(o): return struct.unpack('>H', data[o:o+2])[0]
def s16(o): return struct.unpack('>h', data[o:o+2])[0]
def u32(o): return struct.unpack('>I', data[o:o+4])[0]

sfnt = u32(0)
numTables = u16(4)
tables = {}
for i in range(numTables):
    off = 12 + 16*i
    tag = data[off:off+4].decode('latin-1')
    tables[tag] = (u32(off+8), u32(off+12))
print("tables:", sorted(tables.keys()))

head = tables['head'][0]
unitsPerEm = u16(head+18)
indexToLocFormat = s16(head+50)
hhea = tables['hhea'][0]
asc = s16(hhea+4); desc = s16(hhea+6); gap = s16(hhea+8)
numH = u16(hhea+34)
print("unitsPerEm", unitsPerEm, "asc", asc, "desc", desc, "gap", gap, "numberOfHMetrics", numH)

hmtx = tables['hmtx'][0]
def adv_units(gid):
    if gid < numH:
        return u16(hmtx + 4*gid)
    return u16(hmtx + 4*(numH-1))

# fvar
if 'fvar' in tables:
    f = tables['fvar'][0]
    axesArrayOffset = u16(f+4)
    axisCount = u16(f+8)
    axisSize = u16(f+10)
    for i in range(axisCount):
        a = f + axesArrayOffset + i*axisSize
        tag = data[a:a+4].decode('latin-1')
        mn = struct.unpack('>i', data[a+4:a+8])[0]/65536.0
        df = struct.unpack('>i', data[a+8:a+12])[0]/65536.0
        mx = struct.unpack('>i', data[a+12:a+16])[0]/65536.0
        print("fvar axis", tag, "min", mn, "default", df, "max", mx)

# cmap
cm = tables['cmap'][0]
n = u16(cm+2)
best = None
for i in range(n):
    o = cm + 4 + 8*i
    pid = u16(o); eid = u16(o+2); sub = u32(o+4)
    fmt = u16(cm+sub)
    if (pid, eid) in ((3,10),(3,1),(0,3),(0,4),(0,6)):
        if best is None or (pid,eid)==(3,10):
            best = (cm+sub, fmt)
sub, fmt = best
print("cmap format", fmt)
cmap = {}
if fmt == 4:
    segX2 = u16(sub+6); seg = segX2//2
    endo = sub+14; starto = endo+segX2+2; deltao = starto+segX2; rangeo = deltao+segX2
    for i in range(seg):
        end = u16(endo+2*i); start = u16(starto+2*i)
        delta = s16(deltao+2*i); ro = u16(rangeo+2*i)
        for c in range(start, min(end, 0xFFFE)+1):
            if ro == 0:
                g = (c + delta) & 0xFFFF
            else:
                gi = rangeo+2*i + ro + 2*(c-start)
                g = u16(gi)
                if g: g = (g+delta) & 0xFFFF
            if g: cmap[c] = g
elif fmt == 12:
    ngroups = u32(sub+12)
    for i in range(ngroups):
        o = sub+16+12*i
        s = u32(o); e = u32(o+4); g = u32(o+8)
        for c in range(s, e+1):
            cmap[c] = g + (c-s)

ppem = 12.0  # 9pt at 96dpi
scale = ppem/unitsPerEm

def adv_px(ch):
    g = cmap.get(ord(ch))
    if g is None:
        return None
    return adv_units(g)*scale

table = {}
for ch in " abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,:;()=-'\"?/":
    a = adv_px(ch)
    table[ch] = a

print("advances (px, unrounded / rounded):")
for ch in sorted(table):
    a = table[ch]
    print(repr(ch), round(a,3), round(a))

# max digit width
maxdig = max(table[c] for c in "0123456789")
print("maxDigitWidth", maxdig)

def width(s, weight_scale=1.0):
    # emulate LLFontGL::getWidthF32: sum advances, round after each char (no kerning table in Inter)
    cur = 0.0
    for ch in s:
        a = table.get(ch)
        if a is None:
            a = 7.0
            print("  !! missing glyph", repr(ch))
        if ch.isdigit():
            a = maxdig
        cur += a*weight_scale
        cur = float(round(cur))
    return cur

strings = [
 ("ROC intro", "Remembers the lasting scenery of a region and paints it back when you return."),
 ("SQ intro", "Keeps textures compressed on the graphics card, saving video memory."),
 ("ROC hdr1", "How quickly scenery is trusted"),
 ("ROC hdr2", "Limits"),
 ("SQ hdr", "Limits"),
 ("cb roc enable", "Enable the region object cache"),
 ("cb roc proof", "Only cache objects that cannot be auto-returned"),
 ("cb sq enable", "Use GPU-compressed textures"),
 ("cb sq bg", "Build compressed textures in the background"),
 ("cb sq net", "Finish downloading textures so they can be compressed"),
 ("sl roc 1", "Hours that count as a return visit:"),
 ("sl roc 2", "Visits before caching:"),
 ("sl roc 3", "Days before caching the rest:"),
 ("sl roc 4", "How permanent it must look:"),
 ("sl roc 5", "Objects remembered per region:"),
 ("sl roc 6", "Objects drawn from memory on arrival:"),
 ("sl roc 7", "Disk space for the cache (MB):"),
 ("sl roc 8", "Seconds to keep unconfirmed scenery:"),
 ("sl sq 1", "Disk space for compressed textures (MB):"),
 ("sl sq 2", "Processor cores to use (0 = automatic):"),
]
print()
for name, s in strings:
    print("%-16s len=%3d  regular=%6.1f px   bold~=%6.1f px   | %s" % (name, len(s), width(s), width(s,1.06), s))
