import xml.etree.ElementTree as ET

sx = r'D:\soapstorm\indra\newview\app_settings\settings.xml'
root = ET.parse(sx).getroot()
top = root if root.tag == 'map' else root.find('map')

def parse_map(m):
    out = {}
    kids = list(m)
    i = 0
    while i + 1 < len(kids) + 1 and i < len(kids):
        k = kids[i]
        if k.tag != 'key':
            i += 1
            continue
        v = kids[i + 1]
        out[k.text] = v
        i += 2
    return out

settings = parse_map(top)
print('settings parsed:', len(settings))

def val_of(node):
    m = parse_map(node)
    t = m.get('Type')
    v = m.get('Value')
    tv = t.text if t is not None else None
    if v is None:
        return tv, None
    if len(list(v)):
        return tv, '<complex>'
    return tv, (v.text or '').strip()

live = r'D:\soapstorm\indra\newview\skins\default\xui\en'
for f in ['panel_preferences_ss_roc.xml', 'panel_preferences_ss_squeeze.xml']:
    print('##', f)
    t = ET.parse(live + '\\' + f)
    for el in t.getroot().iter():
        cn = el.attrib.get('control_name')
        if not cn:
            continue
        node = settings.get(cn)
        if node is None:
            print('   *** MISSING FROM settings.xml:', cn)
            continue
        typ, val = val_of(node)
        mn = el.attrib.get('min_val')
        mx = el.attrib.get('max_val')
        line = '   %-32s tag=%-9s type=%-8s default=%-8s' % (cn, el.tag, typ, val)
        if mn is not None:
            try:
                v = float(val)
                lo = float(mn); hi = float(mx)
                line += ' range=[%s..%s] %s' % (mn, mx, 'ok' if lo <= v <= hi else '*** DEFAULT OUT OF RANGE ***')
            except Exception as e:
                line += ' range=[%s..%s] cmpfail:%s' % (mn, mx, e)
        if el.tag == 'check_box' and typ != 'Boolean':
            line += ' *** CHECKBOX ON NON-BOOLEAN ***'
        if el.tag == 'slider' and typ not in ('U32', 'S32', 'F32'):
            line += ' *** SLIDER ON %s ***' % typ
        print(line)
