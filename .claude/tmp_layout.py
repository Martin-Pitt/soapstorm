import xml.etree.ElementTree as ET
import sys

VPAD = 4

def sim(path, W, H, label):
    tree = ET.parse(path)
    root = tree.getroot()
    print("=== %s  (panel local rect W=%d H=%d) ===" % (label, W, H))
    print("%-3s %-12s %-28s %6s %6s %6s %6s  %s" % ("#", "tag", "name/ctrl", "left", "right", "yTop", "yBot", "attrs"))
    prev = None  # (mLeft,mTop,mRight,mBottom)
    rows = []
    idx = 0
    for el in root:
        if not isinstance(el.tag, str):
            continue  # comment
        a = el.attrib
        idx += 1
        height = int(a['height']) if 'height' in a else None
        width = int(a['width']) if 'width' in a else None
        left_p = 'left' in a
        top_p = 'top' in a
        bottom_p = 'bottom' in a
        right_p = 'right' in a

        left = int(a['left']) if left_p else None
        top = int(a['top']) if top_p else None

        # layout_rect = parent local rect: mLeft=0 mBottom=0 mRight=W mTop=H
        LR_L, LR_B, LR_R, LR_T = 0, 0, W, H
        if left_p:
            left = left + (LR_L if left >= 0 else LR_R)
        if top_p:
            top = top + (LR_B if top >= 0 else LR_T)
            top = LR_B + LR_T - top   # topleft invert

        if height is None:
            if not top_p:
                height = 10  # MIN_WIDGET_HEIGHT
            else:
                height = None

        # default_rect
        if prev is None:
            dr = [0, 2*H, W, H]  # L,T,R,B  (parent rect translated by +H)
        else:
            dr = list(prev)

        dr_h = dr[1] - dr[3]
        dr_w = dr[2] - dr[0]

        bd_provided = False
        if 'bottom_delta' in a:
            bd = -int(a['bottom_delta']); bd_provided = True
        elif 'top_pad' in a:
            bd = -(height + int(a['top_pad'])); bd_provided = True
        elif 'top_delta' in a:
            bd = -(int(a['top_delta']) + height - dr_h); bd_provided = True
        elif 'left_delta' not in a and 'left_pad' not in a:
            bd = -(height + VPAD)  # not provided
        else:
            bd = 0  # not provided

        ld_provided = 'left_delta' in a
        if ld_provided:
            ld = int(a['left_delta'])
        elif 'left_pad' in a:
            ld = int(a['left_pad']) + dr_w  # not provided
        else:
            ld = 0  # not provided

        dr[0] += ld; dr[2] += ld
        dr[1] += bd; dr[3] += bd

        # if bottom_delta provided, drop explicit bottom; if left_delta provided, drop explicit left
        if bd_provided:
            bottom_p = False
        if ld_provided:
            left_p = False

        if not left_p: left = dr[0]
        bottom = dr[3] if not bottom_p else None
        if not top_p: top = dr[1]
        right = dr[2] if not right_p else int(a['right'])
        if width is None: width = dr_w
        if height is None: height = dr_h

        # llui.cpp rect resolution (left/width preferred, bottom/height preferred)
        if left_p and right_p:
            mLeft, mRight = left, right
        elif width is not None:
            if right_p and not left_p:
                mRight, mLeft = right, right - width
            else:
                mLeft, mRight = left, left + width
        else:
            mLeft, mRight = left, right

        if bottom_p and top_p:
            mBottom, mTop = bottom, top
        elif height is not None:
            if top_p:
                mTop, mBottom = top, top - height
            else:
                mBottom, mTop = bottom, bottom + height
        else:
            mBottom, mTop = bottom, top

        prev = (mLeft, mTop, mRight, mBottom)
        yTop = H - mTop
        yBot = H - mBottom
        nm = a.get('control_name') or a.get('name') or (el.text or '').strip()[:24]
        extra = " ".join("%s=%s" % (k, a[k]) for k in ('top', 'top_pad', 'top_delta', 'left', 'width', 'height') if k in a)
        rows.append((idx, el.tag, nm, mLeft, mRight, yTop, yBot, extra, (el.text or '').strip()))
        print("%-3d %-12s %-28s %6d %6d %6d %6d  %s" % (idx, el.tag, nm[:28], mLeft, mRight, yTop, yBot, extra))

    print()
    # overlap analysis: pairwise rect intersection
    problems = []
    for i in range(len(rows)):
        for j in range(i+1, len(rows)):
            a = rows[i]; b = rows[j]
            xo = min(a[4], b[4]) - max(a[3], b[3])
            yo = min(a[6], b[6]) - max(a[5], b[5])
            if xo > 0 and yo > 0:
                problems.append("OVERLAP: #%d %s(%s) x[%d,%d] y[%d,%d]  vs  #%d %s(%s) x[%d,%d] y[%d,%d]  -> %dx%d px" %
                                (a[0], a[1], a[2], a[3], a[4], a[5], a[6], b[0], b[1], b[2], b[3], b[4], b[5], b[6], xo, yo))
    for i in range(len(rows)-1):
        a = rows[i]; b = rows[i+1]
        gap = b[5] - a[6]
        print("gap after #%d %-24s -> #%d %-24s = %d px" % (a[0], a[2][:24], b[0], b[2][:24], gap))
    print()
    maxb = max(r[6] for r in rows)
    maxr = max(r[4] for r in rows)
    print("content bottom (y from panel top) = %d ; panel H = %d ; slack = %d" % (maxb, H, H - maxb))
    print("content right edge = %d ; panel W = %d ; slack = %d" % (maxr, W, W - maxr))
    for p in problems:
        print(p)
    if not problems:
        print("no pairwise rect overlaps")
    print()
    return rows

base = r"D:\soapstorm\indra\newview\skins\default\xui\en"
for f, lbl in [("panel_preferences_ss_roc.xml", "ROC"), ("panel_preferences_ss_squeeze.xml", "SQUEEZE")]:
    # simulate at declared size first, then at runtime size
    sim(base + "\\" + f, 540, 480, lbl + " (declared 540x480)")
