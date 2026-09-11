import io


def patch(path, pairs):
    s = io.open(path, encoding='utf-8', newline='').read()
    for a, b, n in pairs:
        assert s.count(a) == n, (path, a[:100], s.count(a))
        s = s.replace(a, b)
    io.open(path, 'w', encoding='utf-8', newline='').write(s)
    print('ok', path)


# ---- A5: the ground reach only ever fires for a point inside a body --------------
patch('indra/newview/app_settings/settings.xml', [
    ("<string>How far in metres a query point with no air cell at its own height - a camera pushed inside a wall, an ear inside a floor slab - may walk DOWN looking for one before the world field answers open air. Points in real air (flying, above a sky platform, rain aloft) never use this: their own column's top gap already answers</string>\n      <key>Persist</key>\n      <integer>1</integer>\n      <key>Type</key>\n      <string>F32</string>\n      <key>Value</key>\n      <real>64.0</real>",
     "<string>How far in metres a query point with no air cell at its own height - a camera pushed inside a wall, an ear inside a floor slab - may walk DOWN looking for one before the world field answers open air. Points in real air (flying, above a sky platform, rain aloft) never use this: their own column's top gap already answers. Keep it short: it is a clipping-recovery distance, not a search, and a camera clipped into terrain with a large value here adopts the label of whatever basement it finds</string>\n      <key>Persist</key>\n      <integer>1</integer>\n      <key>Type</key>\n      <string>F32</string>\n      <key>Value</key>\n      <real>6.0</real>", 1),
])

patch('indra/newview/ssworldfield.cpp', [
    ('    static LLCachedControl<F32> reach(gSavedSettings, "SSWorldFieldGroundReach", 64.f);\n'
     '    return llclamp((F32)reach, 1.f, 4096.f);',
     '    static LLCachedControl<F32> reach(gSavedSettings, "SSWorldFieldGroundReach", 6.f);\n'
     '    return llclamp((F32)reach, 0.5f, 64.f);', 1),
])

REACH_TIP = "How far a query point with no air at its own height - a camera clipped into a wall or into terrain - may walk down looking for one before the field answers open air. A clipping-recovery distance, not a search: too large and a clipped camera adopts a basement's label"
patch('indra/newview/skins/default/xui/en/floater_ss_atmo_worldfield.xml', [
    ('tool_tip="How far a query point with no air at its own height - a camera inside a wall, an ear inside a floor slab - may walk down looking for one before the field answers open air"',
     'tool_tip="%s"' % REACH_TIP, 3),
    ('min_val="1" max_val="512" increment="1"', 'min_val="0.5" max_val="64" increment="0.5"', 1),
    ('label_width="0" min_val="1" max_val="512" increment="1" decimal_digits="0"',
     'label_width="0" min_val="0.5" max_val="64" increment="0.5" decimal_digits="1"', 1),
])

# ---- A7: the stale sentence the acoustics agent would read as its brief -----------
patch('doc/atmo_magic_acoustics.md', [
    ("from the flood's gap depth × cell size",
     "from the classification's covered distance (DECIMETRES since 2026-09-11, so metres = depth x 0.1 - it used to be graph hops x cell size and so moved with SSWorldFieldCell)", 1),
])

# ---- A7: sheetsWanted's comment overstated when it is read -----------------------
patch('indra/newview/ssnavmesh.cpp', [
    ("// Whether band builds keep a world-field sheet: the field's master switch alone, so a sheet is either kept by every band or by none.",
     "// <SS:Nexii> Whether band builds keep a world-field sheet: the field's master switch alone, so a sheet is either kept by every band or by none. An LLCachedControl, so it follows a live change - but only for builds launched after it; resheet() is what recovers the bands built under the old answer. [interaction: SSWorldField::update]", 1),
])
