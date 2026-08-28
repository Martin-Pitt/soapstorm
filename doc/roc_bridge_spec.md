# ROC ↔ LSL Bridge: request/reply spec (hand-off to the LSL author)

Status: SPEC FOR IMPLEMENTATION, 2026-08-27. Written for the LSL side to be authored by the owner (Nexii). The viewer side is implemented against this contract; nothing in the viewer assumes anything about the script beyond what is written here.

Companion docs: `doc/region_object_cache.md` (the ROC design), `doc/super_compressed_textures.md` (the BC7 texture tier).

## 1. Intent, and why the bridge is involved at all

The Region Object Cache rezzes previously-seen static objects as suppressed "ghosts" the instant you enter a known region, then reconciles them against what the simulator actually streams. Two questions cannot be answered from the viewer's own data, because the simulator's interest list deliberately withholds objects the viewer is not looking at:

1. **Does object K still exist in this region?** Viewer silence is not evidence — an object may simply not be streamed. Only an affirmative answer settles it. This is what makes stale ghosts cullable and what feeds the error budget that trashes a bad cache.
2. **How long has object K stood in this region?** The viewer protocol has no rez time at all (verified: `message_template.msg` has no such field; `ObjectProperties.CreationDate` is asset creation and survives take-and-re-rez). `OBJECT_REZ_TIME` is LSL-only.

A third question is *mostly* answerable in the viewer but has one gap:

3. **Is object K immune to parcel auto-return?** The viewer can already establish this for most objects on its own (object owner UUID is inside the cached object blob; parcel owner/group and the parcel's auto-return time come from `ParcelProperties`). The residue the viewer cannot see is whether an object is **set to the parcel's group** — that lives only in `ObjectProperties`, which costs one select per object. In bulk, the script can answer it directly.

**Minimality principle: the bridge is asked only what only it can answer.** Everything derivable viewer-side is done viewer-side. The script holds no per-object state, no lists between calls, and no session memory. Every request is self-contained and every reply is a pure function of that request. If a request is dropped, the viewer simply asks again later; nothing desynchronises.

## 2. Hard constraints (verified against this tree)

REQUEST DIRECTION
- `viewerToLSL` serialises the payload as an LLSD String via `LLSDSerialize::toXML` (fslslbridge.cpp:608 → llcorehttputil.cpp:144), producing exactly `<llsd><string>` (14 bytes) + payload + `</string></llsd>\n` (17 bytes) = **31 bytes of fixed wrapper**. The script's fixed-offset unwrap `llGetSubString(Body, 14, llStringLength(Body) - 18)` (script:524) matches that byte-for-byte.
- XML escaping only expands `< > & ' "`. Hex digits, dashes, commas and pipes pass through 1:1, so payload arithmetic is exact if the wire alphabet is restricted to those.
- The **2048-byte `http_request` body cap is believed but NOT verifiable from this tree.** Its only trace is the comment at fsradar.h:39, whose own arithmetic does not produce the constant it annotates. Treat it as a runtime-measured value, not a constant of nature (see mode `p`).
- CONFIRMED BUG in existing code, worth fixing in the same pass: the radar's batch loop (fsradar.cpp:662-675) flushes on `++updatesPerRequest > 60`, i.e. sends **61** dashed UUIDs ≈ 2299 bytes — over the believed cap. Truncation is doubly harmful because the fixed-offset unwrap then discards 17 *further* characters of real data silently.

REPLY DIRECTION
- The viewer imposes **no reply size limit at all**: the LLSD XML parse path is explicitly `SIZE_UNLIMITED` (llsdserialize.h:809-813) and streams through expat in 1 KB chunks; the only numeric constant is `MAX_BODY_SIZE_THRESHOLD = 65536` (llcorehttputil.cpp:299), which merely moves >64 KB parses off the main thread.
- Firestorm's own shipping `getZOffsets` deliberately constructs **~2.8–3.1 KB** `llHTTPResponse` bodies, so FS does not believe the 2048 figure applies to replies.
- The true SL-side reply cap is unproven here. This spec is therefore designed to be correct whether the cap turns out to be 2 KB or effectively unbounded.

TRANSPORT SHAPE
- Replies MUST use `llHTTPResponse` (synchronous, correlated to the exact request via the viewer's per-request closure). Do NOT use `llOwnerSay` for ROC: it is uncorrelated, spoofable by any object the user owns, and bounded by the ~1023-byte chat limit.
- The bridge URL becomes valid only seconds after region entry and goes stale on region crossing without being cleared, so viewer sends can fail silently. The viewer gates every sweep on a bridge-ready signal and retries on the next handshake. The script needs no special handling for this.

SCRIPT MEMORY (the owner's stated concern)
- The bridge script lives in a 64 KB Mono allocation shared with movelock, flight assist, AO, radar offsets and the combat listeners. ROC must be a guest, not a tenant: no globals, no persistent lists, no accumulation across events.
- The recommended shape is a **single streaming pass**: iterate the request string, extract one key at a time, query it, append at most a few characters to one output string, and never materialise a second list the size of the input. Whatever the LSL author judges cheapest is fine — the wire contract below does not require any particular internal strategy.
- The viewer will never send more than `K` keys in one request, and `K` is a viewer-side setting the owner can lower at any time without a script change.

## 3. Wire format

All ROC traffic uses the single command word `RocSweep`, so exactly one new `else if` branch is added to the dispatcher.

### Request

```
RocSweep|<mode>|<sid>|<n>|<key1>,<key2>,...,<keyN>|END
```

| Field   | Meaning |
|---------|---------|
| `mode`  | One of `e`, `r`, `a`, `p` (below). |
| `sid`   | Opaque viewer token, ≤4 chars, echoed verbatim so both sides' logs can be matched and so a stale reply can be discarded. |
| `n`     | Number of keys the viewer *intended* to send. |
| keys    | Dashed UUIDs, comma-separated, no spaces. |
| `END`   | Literal sentinel. Its absence means the request was truncated in transit. |

**Truncation rule (load-bearing):** if the trailing `END` is missing, or the parsed key count ≠ `n`, the script MUST answer `ROC|<sid>|0|0|T|END` and MUST NOT answer partially. The viewer treats that as *neutral* — no evidence gained, nothing counted against any object. This is what makes it impossible for a truncated request to manufacture a false "object is missing" verdict, which would otherwise erode or trash a good cache.

### Reply

```
ROC|<sid>|<checked>|<present>|<mode>|<csv>|END
```

| Field      | Meaning |
|------------|---------|
| `checked`  | How many keys the script actually examined. Must equal `n`. |
| `present`  | How many of those were found present. |
| `mode`     | Echoed mode, or an error token: `T` = request clipped / count mismatch, `OVER` = the answer exceeded the script's own reply budget. |
| `csv`      | For mode `e`: **the missing keys themselves, in full, dashed** — the owner's amendment. For mode `r`: `<index>:<age_seconds>` pairs. For mode `a`: the keys that are NOT auto-return-proof. Empty is legal. |
| `END`      | Literal sentinel; its absence means the reply was truncated, and the viewer discards it as neutral. |

**Why full keys are free — and why indexes are not needed after all.** Using a guaranteed one-character separator, a request of K dashed keys is `37K + 53` bytes on the wire including the 31-byte wrapper. A reply can carry at most `K-2` missing keys (two canary slots are mandatory, below), so the worst-case reply is `37K − 24` bytes. **The answer can never be larger than the question that produced it, for any K.** That makes the unverified reply-side cap irrelevant to correctness and means echoing full keys costs nothing versus indexes — while being immune to any request/reply misalignment. At K = 50: request 1903 B, worst reply 1826 B; both under 2048, so one wire format is correct whether the real cap is 2048 or unbounded.

**Script-side self-limit.** Keep an `integer ROC_REPLY_BUDGET = 1800;`. If an assembled payload would exceed it, answer `ROC|<sid>|<checked>|<present>|OVER|<found>|END` *without* the list; the viewer re-asks as two halves and converges in ≤ log2(K) steps with **zero continuation state** in the 64 KB heap. At K = 50 in mode `e` this branch is unreachable by the theorem above — it exists for mode `r` and any future mode.

### The six acceptance guards

A reply is allowed to refute anything only if **all six** hold. Each closes a hole that would otherwise let a transport accident delete a user's cache:

1. **END sentinel** — checked *from the end of the list*, because `llParseString2List` drops empty tokens, so a fixed index shifts when the missing list is empty.
2. **Count echo** — `checked == n`.
3. **Conservation** — `present + missing_count == checked == n`. Without this, "every unlisted key is present" is an argument from pure absence: `ROC|999|50|e||END` looks identical whether the script checked 50 objects and found them all, or its result list came back empty from a bug.
4. **Canary anchors** — every batch carries 2 keys the viewer independently confirmed live *this session*. Canaries present means the batch was genuinely resolving objects in this region, so even a 100%-missing result is trustworthy; a missing canary voids the whole reply.
5. **Membership** — every returned key must be one the viewer actually sent. A single stranger key voids the reply.
6. **Context** — `sid` matches the in-flight request *and* the agent's region handle is unchanged since the send. The bridge is an attachment and crosses regions with the avatar; without this, a sweep posted in region A but executed after crossing to region B resolves nothing and would report 100% missing.

Everything else — no reply, late reply, `OVER`, empty body, failure callback, bridge absent — is **neutral**. There is no path from a transport problem to a refutation.

## 4. Modes

### `e` — existence sweep (primary, highest value)

Predicate: *the object still exists in this region.*

For each key, ask the simulator whether the object is present — `llGetObjectDetails` with any cheap parameter is the obvious route; whatever proves presence most cheaply is the LSL author's call. Return the missing keys in full; the count of present keys goes in the `present` field.

Evidence contract, as the viewer will interpret it:
- An index reported **absent** in a well-formed reply with `checked == n` is an **absolute confirmation of non-existence**. The viewer evicts that ROC record and counts one refutation toward the trash-the-region threshold. This is the only mechanism in the entire ROC design that produces certainty about deletion — everything else is inference.
- An index reported **present** is an absolute confirmation of existence and refreshes that record's tenure.
- Anything else — no reply, late reply, error reply, `checked != n`, missing `END` — is **neutral**. Never evidence in either direction.

Known caveat to confirm empirically: an object that has crossed into a neighbouring region reads as absent from this region's perspective. The viewer already down-weights refutations for objects sitting within 10 m of a region border for this reason.

### `r` — rez-time sweep (persistence evidence)

Predicate: *how long has each object stood in this region?*

The CSV carries `<index>:<age_seconds>` pairs for keys still present, indexes being positions in the request list. Ages are more informative than a yes/no for tuning the promotion threshold, and the `OVER` budget rule above covers the case where a full batch of pairs would exceed the reply budget — the viewer just re-asks in halves. If you would rather answer a threshold yes/no to save script cycles, say so and the viewer will send the threshold in the mode token instead; both shapes are cheap to support on the viewer side.

Unlike mode `e`, this mode never refutes anything: a key absent from the pair list is treated as "age unknown", not "object gone". Only mode `e` produces refutations.

Open item for the LSL author's judgement: whether `OBJECT_REZ_TIME` survives a region restart. If it resets on restart, this mode still works but reports young ages after every restart, and the viewer's bridge-free cross-visit tenure ledger carries the weight instead. The viewer never treats rez-time as its sole promotion evidence for exactly this reason.

### `a` — auto-return-proof sweep (the ownership gap)

Predicate: *the object cannot be auto-returned from the parcel it is standing on* — i.e. its owner is that parcel's owner, or it is set to that parcel's group, or that parcel has auto-return disabled.

This is the one the owner specifically described, and the script can answer it exactly where the viewer cannot: for each key, compare the object's owner and group against the owner and group of the parcel at the object's own position. The relevant primitives all exist (`OBJECT_OWNER`, `OBJECT_GROUP`, `OBJECT_POS`, and `llGetParcelDetails` with `PARCEL_DETAILS_OWNER` / `PARCEL_DETAILS_GROUP` — all four confirmed present in this tree's LSL keyword data). The CSV carries the keys that are **not** proof, and `present` carries the count that are — so conservation still holds and, in the common case where nearly everything on a parcel is immune, the reply is nearly empty.

**Why this mode earns its place, given the viewer can mostly do it alone.** The viewer's own path costs real sim-visible traffic: one `ParcelPropertiesRequest` per parcel per visit to learn each parcel's owner, group and auto-return timer, plus a batched `ObjectSelect` probe for every object whose "set to group" status is unknown — because there is no group-set bit anywhere in the object update, and `FLAGS_OBJECT_GROUP_OWNED` means *deeded*, which is a different thing. Region-wide parcel enumeration plus bulk object selects, running automatically on every region entry, is real sim load landing at exactly the moment the region is streaming the updates this feature exists to accelerate.

An earlier draft of this paragraph called that a scraper-shaped profile an estate's anti-copybot heuristics may well flag. **That was wrong** — LSL has no event for either message, and the stock viewer already sends unattended per-parcel `ParcelPropertiesRequest` on mouse-over and unprompted `ObjectSelect` pairs from `PermissionsTracker`. The full correction is decision 8 in `region_object_cache.md`. The cost is bandwidth and contention, and the answer to bandwidth and contention is a throttle plus persistence, not a bridge.

**One `a` sweep replaces that entire apparatus** where the bridge is available: no parcel enumeration, no select probes, no selection-adjacent messages at all — just one HTTP round trip to a script the user is already wearing. That is a large reduction in sim load, and a larger one in time-to-verdict: one round trip against a metered burst spread across a region entry, with the answer available before the first promotion pass rather than after several visits of it. That is why the mode is worth the script cycles. It is an **accelerator, not a prerequisite** — the viewer keeps its own path for OpenSim, bridge-off and script-mute estates, and since 2026-08-28 that path is the default rather than the fallback.

Note also that a long confirmed tenure is *retrospective* proof of the same thing: auto-return timers are measured in minutes, so an object confirmed present on several distinct days across a span of days has demonstrably not been auto-returned. Mode `a` is the *predictive* confirmation that it will keep standing; tenure is the empirical fallback when nothing can confirm it.

### `p` — calibration probe (recommended, trivial)

```
RocSweep|p|<sid>|0|<filler>|END
```

Reply: `ROC|<sid>|<received_length>|0|p||END`, where `received_length` is `llStringLength` of the body the script actually received.

One round trip turns both unverified size caps into measured runtime constants: the viewer sends a known-length filler, compares, and learns the true request cap; a filler-length binary search establishes the reply cap the same way. This removes the only two guesses in the whole transport and costs one request at bridge version-bump time.

## 5. What the viewer guarantees to the script

- Never more than `K` keys per request (`K` is a viewer setting, initial default 50 dashed UUIDs = 1903 B request / 1826 B worst-case reply — inside the believed cap in *both* directions even if every key comes back in the CSV).
- Two of every batch's keys are canaries the viewer has independently confirmed live this session; the script needs to do nothing special with them, they are ordinary keys.
- Sweeps only after the bridge-ready signal for the current region, never during a region crossing.
- A hard cadence cap (initial default: at most one sweep request per 7 seconds, matching the radar's tuned envelope), and no sweeps at all while the viewer is in the teleport/login burst.
- Existence sweeps are prioritised oldest-ghost-first so answers land inside the ghost TTL window; rez-time and ownership sweeps are strictly background and may be starved indefinitely without harm.
- The viewer treats *every* anomalous outcome as neutral. There is no reply the script can send — including no reply at all — that damages the user's cache. The script cannot be made responsible for correctness.

## 6. Open items for the LSL author

1. Cheapest presence test per key for mode `e`, and whether one `llGetObjectDetails` call can carry multiple keys' worth of work in your judgement.
2. Whether mode `r` should return `<index>:<age_seconds>` pairs as specified, or a threshold yes/no, given script-side string cost.
3. Whether mode `a` should fold in a parcel-level short-circuit (all keys on one parcel with auto-return off → answer in one comparison) or whether the viewer should pre-group keys by parcel before sending. The viewer can do the grouping if that saves script work — say which you prefer.
4. Practical `K` ceiling once you have measured `llGetFreeMemory()` inside the handler with movelock, flight assist and AO active.
5. Whether the `OVER` halving protocol is acceptable as the only overflow mechanism. It was chosen over a continuation token specifically because it needs **zero** state carried between events in the 64 KB heap.
6. Version bump mechanics: this adds one dispatcher branch and no new stateful mode, so no re-arm entry is needed in the handshake block. Bump `BRIDGE_VERSION` and the viewer-side minor version together as usual.
