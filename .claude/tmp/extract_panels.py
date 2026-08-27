import json, os

src = r'C:\Users\nexii\.claude\projects\D--soapstorm\36b42fa6-9e0c-441c-92a2-0f6eb7d8e0cb.jsonl'
out = r'D:\soapstorm\.claude\tmp\panels'
os.makedirs(out, exist_ok=True)
seq = {}
i = 0
for line in open(src, encoding='utf-8'):
    i += 1
    try:
        d = json.loads(line)
    except Exception:
        continue
    m = d.get('message')
    if not isinstance(m, dict):
        continue
    c = m.get('content')
    if not isinstance(c, list):
        continue
    for b in c:
        if isinstance(b, dict) and b.get('type') == 'tool_use' and b.get('name') == 'Write':
            inp = b.get('input', {}) or {}
            fp = str(inp.get('file_path', ''))
            if 'panel_preferences_ss' in fp or 'panel_preferences_soapstorm' in fp:
                base = os.path.basename(fp.replace('\\', '/'))
                seq[base] = seq.get(base, 0) + 1
                p = os.path.join(out, base + '.v%d' % seq[base])
                with open(p, 'w', encoding='utf-8', newline='') as fh:
                    fh.write(inp.get('content', ''))
                print('wrote', p, 'from jsonl line', i)
