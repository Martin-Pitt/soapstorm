import xml.etree.ElementTree as ET

VPAD = 4
AVAIL_H = 450   # sub-tab body height, computed from floater 561 -> pref core 480 -> soapstorm panel 478 -> sub tab_container 472 -> body 450
AVAIL_W = 537

live = r'D:\soapstorm\indra\newview\skins\default\xui\en'

for f in ['panel_preferences_ss_roc.xml', 'panel_preferences_ss_squeeze.xml']:
    print('#' * 60)
    print('#', f)
    root = ET.parse(live + '\\' + f).getroot()
    boxes = []
    prev = None
    for el in root:
        a = el.attrib
        h = int(a.get('height', '0')) if a.get('height') else None
        if h is None:
            h = 10
        w = int(a.get('width', '0')) if a.get('width') else 0
        left = int(a.get('left')) if a.get('left') is not None else (prev['left'] if prev else 0)
        if 'top' in a:
            top = int(a['top'])
        elif 'top_pad' in a:
            top = (prev['bot'] if prev else 0) + int(a['top_pad'])
        elif 'top_delta' in a:
            top = (prev['top'] if prev else 0) + int(a['top_delta'])
        else:
            top = (prev['bot'] if prev else 0) + VPAD
        b = {'tag': el.tag, 'name': a.get('name') or (el.text or '').strip()[:28],
             'left': left, 'right': left + w, 'top': top, 'bot': top + h, 'h': h, 'w': w}
        boxes.append(b)
        prev = b
    for b in boxes:
        flag = ''
        if b['bot'] > AVAIL_H:
            flag += ' *** BELOW PANEL BOTTOM ***'
        if b['right'] > AVAIL_W:
            flag += ' *** PAST RIGHT EDGE ***'
        print('  y %3d..%3d  x %3d..%3d  %-10s %s%s' % (b['top'], b['bot'], b['left'], b['right'], b['tag'], b['name'], flag))
    print('  content bottom =', max(b['bot'] for b in boxes), ' available =', AVAIL_H)
    print('  -- overlaps --')
    n = 0
    for i in range(len(boxes)):
        for j in range(i + 1, len(boxes)):
            A, B = boxes[i], boxes[j]
            vy = min(A['bot'], B['bot']) - max(A['top'], B['top'])
            vx = min(A['right'], B['right']) - max(A['left'], B['left'])
            if vy > 0 and vx > 0:
                n += 1
                print('    OVERLAP %s/%s vs %s/%s  x%d y%d' % (A['tag'], A['name'], B['tag'], B['name'], vx, vy))
    if n == 0:
        print('    none')
