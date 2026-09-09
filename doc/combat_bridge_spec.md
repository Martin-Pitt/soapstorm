# Combat Log bridge wire contract

Contract between the Soapstorm viewer and the second bridge script (`indra/newview/fs_resources/ssLSLBridge.lsltxt`). The viewer side is fixed by this document; the LSL is the owner's. Design rationale lives in `doc/combat_log_ux.md`, `doc/combat_log_analysis.md` and the engineering plan; this file is only the bytes on the wire.

## 1. Where the two scripts sit

- Both scripts live in the bridge prim. The main script (`EBEDD1D2-…lsltxt`) owns the single HTTP URL; `http_request` fires in every script in the prim, so the second script answers the commands listed as *no-ack* below with its own `llHTTPResponse`, and the main script must `return` without its generic `ok` for those.
- The viewer names the second script's inventory item `#Soapstorm LSL Bridge v<major>.<minor>` with the same numbers as the main script. Bridge version becomes **2.34**.
- Required main-script edits: whitelist that name in the `CHANGED_INVENTORY` injection guard (`n > 2`, skip both names); bump `BRIDGE_VERSION` to `2.34`; `return` before the generic ack for `GetTrackedAgents` (done) and `CombatObjectInfo`.
- Everything the second script pushes is `llOwnerSay` from the bridge prim; the viewer accepts it only from the prim UUID it shook hands with.

## 2. Commands (viewer → script, POST body `<llsd><string>…</string></llsd>`, ≤ 2048 bytes)

| Command | Handled by | Reply |
|---|---|---|
| `CombatLogListen\|<0/1>` | script 2: `llListen(COMBAT_CHANNEL, "", "", "")` on/off | main acks `ok` |
| `TrackAgents\|<0/1>\|<hz>` | script 2: tracking timer on/off at `hz` samples/s (viewer sends 2) | main acks `ok` |
| `GetTrackedAgents\|<since_seq>` | script 2 | **no-ack**; body in §5 |
| `GetRegionSettings` (optional) | script 2 | **no-ack**; body in §6; the push in §6 makes this unnecessary |
| `CombatObjectInfo\|<k1>,<k2>,…` (≤ 40 keys) | script 2 (optional, later stage) | **no-ack**; body in §7 |

The viewer re-sends `CombatLogListen` and `TrackAgents` on every bridge handshake (login, recreation, region change) while `SSCombatLogEnabled` is on, and sends the `0` forms when it is switched off.

## 3. Frame stamps

Every push and every tick carries `llGetEnv("frame_number")` as a plain integer. One call per batch is enough: the simulator batches combat events per frame already, so stamp the batch, not each element. `final_damage` records carry it too. The viewer fits frame → wall time from the ticks it polls, so frames are the shared clock for ordering hits, deaths and positions; do not send seconds.

## 4. Push: combat log relay (`llOwnerSay`, ≤ 1024 bytes each)

```
C2<frame>|<payload>
```

- `<frame>` is the frame stamp, decimal digits, followed by one `|`.
- **Simulator lines** (`identifier == COMBAT_LOG_ID`): `<payload>` is the message text untouched, a JSON array `[{…},{…}]` or a single object.
- **Foreign lines** (any other sender): `<payload>` is the 36-char sender key, then the 36-char owner key, then the message text. The viewer treats these as untrusted and types them only if they parse to the known schema.
- **`final_damage` records** (the wearer's own incoming hits), one object per detected index, emitted by the attachment itself:
  `{"event":"FINAL_DAMAGE","source":k,"sourceDetails":[name,pos,attach_point,group,creator],"rezzer":k,"rezzerDetails":[rezzer_of_rezzer,name,pos,attach_point,group,creator],"root":k,"rootDetails":[name,pos,attach_point,group,creator],"owner":k,"damage":f,"type":i,"original":f,"targetPos":"<x, y, z>","targetRot":"<x, y, z, s>","targetVel":"<x, y, z>"}`.
  Detail arrays may be empty when the object is gone; vectors inside them and the target fields are LSL vector strings. The viewer maps this to a DAMAGE on the wearer with positions, merges it with its later log twin by (source, owner, type, damage) within 2.5 s, keeps the `final_damage` time and positions, and feeds the detail arrays into the equipment registry.
- Splitting: if `"C2" + frame + "|" + text` would exceed 1024 bytes, forward the array element by element (`llJsonGetValue(text,[i])`) each with the same frame; the viewer salvages complete elements from a truncated array but cannot recover a cut one.
- Optional enrichment the viewer reads when present: `"agent":1` (target is an avatar), `"weapon_name":"…"`.

## 5. Reply: `GetTrackedAgents|<since_seq>`

Body is `<llsd><string>` + lines + `</string></llsd>`. Alphabet is `[0-9a-f.,:;|=\n-]` so nothing needs escaping. Lines:

```
TA|<frame_now>|<seq_from>|<seq_to>|<dropped>
K|<idx>=<uuid>,<idx>=<uuid>,...
T|<seq>|<frame>|<idx>:<x>,<y>,<z>,<yaw>,<vx>,<vy>,<vz>,<flags>[,<root>];<idx>:...;
```

- `TA` first: the current frame, the first and last tick sequence numbers included, and how many ticks were dropped because the ring overflowed since `since_seq` (0 normally).
- `K` (index table) once per reply when `since_seq` is 0, and whenever an agent is added; indices are small integers stable for the session; a removed agent's index is not reused for 60 s.
- `T` one line per tick, ticks in order. Per agent: position to 2 decimals (region-local), `yaw` as integer degrees of the avatar rotation about Z, velocity to 1 decimal, `flags` = `llGetAgentInfo(agent)` as an integer, and an optional `root` key when the agent is seated (`OBJECT_ROOT`), else omitted.
- Delta encoding: include an agent in a tick only if it moved more than 5 cm, its flags changed, or its root changed; send a full snapshot of all agents every 10th tick and on `since_seq` 0.
- The viewer polls every `SSCombatLogPollSeconds` (1 s) with the last `seq` it received and never has two polls in flight. Keep at least 3 s of ticks in the ring; reply size is unbounded by the sim but keep it under ~16 KB.
- Suggested cost budget at 2 Hz for 40 agents: one `llGetAgentList`, one `llGetObjectDetails(agent, [OBJECT_POS, OBJECT_ROT, OBJECT_VELOCITY, OBJECT_ROOT])` and one `llGetAgentInfo` per agent per tick.

## 6. Push: region settings

Pushed once when the URL is granted and on `CHANGED_REGION`:

```
C2RS|{"flags":<llGetRegionFlags>,"allow_damage_adjust":"…","restrict_combat_log":"…","damage_throttle":"…","damage_limit":"…","restore_health":"…","health_regen_rate":"…","invulnerability_time":"…","death_action":"…","agent_limit":"…"}
```

Values are the strings `llGetEnv` returns; `flags` is the region flags integer. The viewer also accepts the same object as a reply body to `GetRegionSettings` (`<llsd><string>RS|{…}</string></llsd>`), which is optional.

## 7. Reply: `CombatObjectInfo|k1,k2,…` (optional)

```
OI|<key>|<name>|<creator>|<owner>|<group>|<attach_point>|<rezzer>|<root>|<x>,<y>,<z>
```

One line per key that still exists (`llGetObjectDetails` with `OBJECT_NAME, OBJECT_CREATOR, OBJECT_OWNER, OBJECT_GROUP, OBJECT_ATTACHED_POINT, OBJECT_REZZER_KEY, OBJECT_ROOT, OBJECT_POS`); names are URL-encoded with `llEscapeURL` so `|` and newlines cannot appear. Keys that no longer exist are omitted. The viewer asks within about half a second of first seeing a key because bullets de-rez quickly.

## 8. Limits and behaviour

- Owner-say 1024 bytes; request 2048 bytes; request id valid 25 s.
- Combat channel volume in a large fight can exceed the 64-event script queue; keep the listen handler to one `llOwnerSay` per event and no other calls, and keep the tracking timer in the same script only if its tick stays under a few milliseconds; otherwise a third script for tracking is acceptable (same naming rule, whitelist it too).
- Nothing in this contract requires FLECS or any region system.

## 9. Test checklist (in-world)

1. Bridge recreates as v2.34 with two running scripts and no injection error in chat.
2. Viewer status line shows `bridge ok`, relay counter rising while someone is shot.
3. `GetTrackedAgents` replies parse: RTT under 300 ms, `dropped` stays 0 with ~20 agents at 2 Hz.
4. A death shows a marker at the DEATH position and the victim's track ends with a spawn teleport.
5. Region settings block appears on the Session page after login and after a region change.
