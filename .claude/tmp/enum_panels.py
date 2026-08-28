import xml.etree.ElementTree as ET, os, sys

base = r'D:\soapstorm\.claude\tmp\panels'
live = r'D:\soapstorm\indra\newview\skins\default\xui\en'

def walk(node, path, acc):
    for i, ch in enumerate(node):
        tag = ch.tag
        a = ch.attrib
        acc.append({
            'tag': tag,
            'name': a.get('name'),
            'control_name': a.get('control_name'),
            'function': a.get('function'),
            'parameter': a.get('parameter'),
            'label': a.get('label'),
            'min': a.get('min_val'), 'max': a.get('max_val'),
            'inc': a.get('increment'), 'dd': a.get('decimal_digits'),
        })
        walk(ch, path + '/' + tag, acc)

def load(p):
    t = ET.parse(p)
    acc = []
    walk(t.getroot(), '', acc)
    return t.getroot().attrib, acc

for f in ['panel_preferences_ss_roc.xml', 'panel_preferences_ss_squeeze.xml']:
    print('#' * 70)
    print('#', f)
    vers = [('v1', os.path.join(base, f + '.v1')), ('v2', os.path.join(base, f + '.v2')), ('DISK', os.path.join(live, f))]
    data = {}
    for label, p in vers:
        root, acc = load(p)
        data[label] = (root, acc)
    for label, _ in vers:
        root, acc = data[label]
        cn = [x['control_name'] for x in acc if x['control_name']]
        nm = [x['name'] for x in acc if x['name']]
        cb = [(x['function'], x['parameter']) for x in acc if x['function']]
        print('--', label, 'panel attrs:', {k: root.get(k) for k in ('name', 'label', 'width', 'height')})
        print('   control_names(%d):' % len(cn), cn)
        print('   names(%d):' % len(nm), nm)
        print('   callbacks:', cb)
    # deltas
    for a, b in [('v1', 'v2'), ('v2', 'DISK'), ('v1', 'DISK')]:
        sa = set(x['control_name'] for x in data[a][1] if x['control_name'])
        sb = set(x['control_name'] for x in data[b][1] if x['control_name'])
        na = set(x['name'] for x in data[a][1] if x['name'])
        nb = set(x['name'] for x in data[b][1] if x['name'])
        ca = set((x['function'], x['parameter']) for x in data[a][1] if x['function'])
        cbs = set((x['function'], x['parameter']) for x in data[b][1] if x['function'])
        print('   [%s->%s] control_name lost:' % (a, b), sorted(sa - sb), ' added:', sorted(sb - sa))
        print('   [%s->%s] name lost:' % (a, b), sorted(na - nb), ' added:', sorted(nb - na))
        print('   [%s->%s] callback lost:' % (a, b), sorted(ca - cbs), ' added:', sorted(cbs - ca))
    # slider param changes v2->DISK
    def key(x):
        return (x['tag'], x['control_name'] or x['name'])
    m2 = {key(x): x for x in data['v2'][1]}
    md = {key(x): x for x in data['DISK'][1]}
    for k in sorted(set(m2) & set(md), key=str):
        for fld in ('min', 'max', 'inc', 'dd', 'label', 'name', 'control_name'):
            if m2[k][fld] != md[k][fld]:
                print('   [v2->DISK] CHANGED', k, fld, repr(m2[k][fld]), '->', repr(md[k][fld]))
