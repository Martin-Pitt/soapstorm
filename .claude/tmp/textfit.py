import xml.etree.ElementTree as ET

adv = {}
for c in 'ijlI':
    adv[c] = 3
for c in 'ft':
    adv[c] = 4
adv['r'] = 5
adv['s'] = 6
for c in 'abcdeghknopquvxyz':
    adv[c] = 7
adv['w'] = 10
adv['m'] = 11
for c in 'EFL':
    adv[c] = 7
for c in 'ABKPRSTVXY':
    adv[c] = 8
for c in 'CDGHNOQU':
    adv[c] = 9
adv['M'] = 11
adv['W'] = 12
for c in '0123456789':
    adv[c] = 8
adv[' '] = 3
adv['J'] = 7
adv['Z'] = 8

PUNCT = 4  # conservative for . , : ( ) - etc


def wid(s, bold=False):
    t = sum(adv.get(c, PUNCT) for c in s)
    return t * (1.06 if bold else 1.0)


def lines(s, width, bold=False):
    words = s.split()
    n = 1
    cur = 0.0
    for w in words:
        ww = wid(w, bold)
        add = ww if cur == 0 else wid(' ', bold) + ww
        if cur + add > width and cur > 0:
            n += 1
            cur = ww
        else:
            cur += add
    return n, cur


live = r'D:\soapstorm\indra\newview\skins\default\xui\en'
for f in ['panel_preferences_ss_roc.xml', 'panel_preferences_ss_squeeze.xml']:
    print('##', f)
    root = ET.parse(live + '\\' + f).getroot()
    for el in root:
        a = el.attrib
        if el.tag == 'text':
            s = ' '.join((el.text or '').split())
            w = int(a.get('width', 0))
            h = int(a.get('height', 0))
            bold = 'Bold' in (a.get('font') or '')
            n, _ = lines(s, w, bold)
            need = 15 * n
            print('  text w=%d h=%d bold=%s est_lines=%d need=%d %s :: %s' %
                  (w, h, bold, n, need, 'OK' if h >= need else '*** TOO SHORT ***', s[:70]))
            if n > 1 and a.get('wrap') != 'true':
                print('      *** would not wrap (wrap!=true) - draws past right edge ***')
        if el.tag == 'slider':
            lbl = a.get('label', '')
            lw = int(a.get('label_width', 0))
            lwid = wid(lbl)
            print('  slider label_width=%d est=%0.0f %s :: %s' %
                  (lw, lwid, 'OK' if lwid <= lw else '*** LABEL WIDER THAN label_width ***', lbl))
        if el.tag == 'check_box':
            lbl = a.get('label', '')
            w = int(a.get('width', 0))
            lwid = wid(lbl) + 20
            print('  check_box w=%d est=%0.0f %s :: %s' % (w, lwid, 'OK' if lwid <= w else '*** LABEL TOO WIDE ***', lbl))
