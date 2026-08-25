# Atmo Magic — lightning design notes

Reference for sslightning*, the thunder half of sssoundscape, and the strike lighting in ssvolcloud/ssVolCloudF. Rationale lives here so the code can carry one-line comments; may drift — code wins.

## Discharge phases (why the timing constants are what they are)
A stepped leader gropes downward in discrete jumps over ~50–180 ms at low brightness — this is the "forking in the sky before it connects" (`mLeaderProgress` gating nodes by `mReachedAt`). On attachment the return stroke fires back UP the channel at ~c/3: microseconds to full brightness, ~55 ms e-fold decay. A flash is typically 1–4 return strokes down the same channel 30–90 ms apart, each dimmer (1/(1+0.6i)) — this restrike pattern is the entire reason lightning flickers rather than flashing once, and is the single most load-bearing realism detail.

## Timing architecture: strikes are known before they happen
`spawn()` runs `PREPARE_LEAD_S` (10 s) before `mFireAt`; `mT` = now − fireAt is negative while pending. This exists for audio: thunder is queued with the *future* fire time and the soundscape works backwards — travel = distance/c, minus the recording's own onset offset — so for a near strike the file's leading air starts *before* the flash. The rejected alternatives both truncated the approach to the bang (start late + skip in) or corrupted the delay per-asset (ignore preamble). It also enables the anticipation charge (`mCharge` ramps over `SSAtmoLightningAnticipation` seconds, ending when the leader starts) — a fantasy effect, off by default.

## Channel generation
Midpoint displacement (self-similar kinks at every scale; a uniform step walk has exactly one scale and reads as zigzag ornament), deliberately unsmoothed — real channels are straight runs meeting at hard angles. Displacement is in the plane perpendicular to the channel's own axis: world-XY jitter is only right for vertical bolts and gives a horizontal in-cloud fork zero vertical wander. Small along-axis jitter makes step lengths uneven. Recursive branches (`SSAtmoLightningBranchDepth`, default 3) inherit the parent's travel direction deviated (leaders branch the way they're going, never upward/backward), each generation shorter/thinner/fewer. Distance LOD 6→3 subdivision levels (800 m → 6 km). `MAX_CHANNEL_NODES` 700 caps the exponential.

## Attachment
Ground strikes land on the flowmap height capture's best `height − lateral × SSAtmoLightningAttachBias` within 120 m. Bias 1.0 = the one-metre-of-reach-per-metre-of-height rule real lightning protection uses. The winner is the first thing inside the leader's reach, NOT the tallest in view — a modest roof underfoot beats a spire 100 m off, which is the entire behaviour.

## Ribbon lightning
The ionised channel is hot air and drifts with the wind between strokes; successive strokes fire down displaced copies. Renderer draws the channel once per still-glowing stroke offset by `windXY × mStrokeAt × SSAtmoLightningRibbonDrift`. Still air: copies coincide. Drift 1.0 is physically honest, 2–3 legible at distance.

## In-cloud compositing and lighting
Bolts draw BEFORE the puff pass; puffs alpha-blend over the channel, so the field itself is the diffuser (dense cloud → glow, gap → bare channel, below base → naked). No occlusion is computed; the *pass ordering in pipeline.cpp is the feature*. Strikes also enter the puff shader as point lights (`ss_strike[4]`) with their own wrapped sphere term per fragment — that is sheet lightning. Using the sun's wrap term instead (first attempt) dimmed the underside of a night deck exactly where a strike below should light it. Channels span SSVolCloud's live band, not constants.

## Scene light
`sceneLights()` appends strikes to the deferred pipeline's `fullscreen_lights` — ordinary local lights, same falloff/batching/shader as everything, which is why avatars and wet ground light correctly with zero new shading code. Light sits at the channel node nearest the camera (the part lighting your street is the part beside you; the origin up in the cloud lights from a direction nothing visible agrees with). Back of queue, max 4: at light budget, a 50 ms flash is the thing least missed.

## Visibility
Bolts render at ANY distance - draw distance governs what the sim sends, not viewer-side effects. Strikes beyond renderFarPlane*0.75 are SQUASHED radially toward the camera (p' = cam + (p-cam)*s, widths/radii * s), the same fit-inside-farclip trick the sky dome uses: angular size preserved exactly, nearer geometry still occludes, the depth-squashed dome stays behind. Audio-only exists only where physics puts it (the ~20km refraction shadow zone, mAudible). Frustum cull (per-frame, renderer only): a strike wholly off screen draws no geometry, but the scene light and in-cloud glow KEEP running - a strike behind the camera lighting the world in front of it is the correct look, and that lit world is what reflections catch (probes never see bolt geometry: gCubeSnapshot guard + probe cadence vs a 50ms flash). The cull sphere folds in the flash-disc radius, so nothing that could touch the frame can be skipped. Scene lights skip only when distance - radius > renderFarPlane (no gbuffer fragment reachable - a wasted batcher slot).

## Glow
Screen alpha IS the glow buffer (`glowExtractF` reads col.a). The lightning pass is the only Atmo pass that writes alpha on purpose (`SSAtmoLightningGlow`, default 0.15 = peak of ONE quad; additive accumulation across overlaps/strokes raises it further, so tune down not up). Sheath gets 30% (blooming the halo instead of the channel is backwards), sparks 50%.

## Thunder acoustics
Thunder is generated along the whole km-long channel simultaneously; the spread of arrival times across its length IS the rumble (near end sharp = crack). Air absorption rises ~f², killing the crack over a few km while the roll carries. Hence crack + rumble are **layered per strike**, not near/far variants: crack gain fades 1.5→6 km, rumble delayed by a channel-depth term (2–5 km / c, ×intensity) that shrinks as the crack dominates. Speed of sound = 331.3 + 0.606·T from the env's temperature. Beyond ~20 km the refraction shadow zone makes strikes correctly silent (`mAudible`). `windCarryGain`: downwind refraction bends sound back to the ground (carries further), upwind up and away (muffled to a 0.25 floor, never erased) — grows with distance and wind, saturating at a gale.

## Onset detection (llaudio, `getOnsetMS`)
10 ms RMS envelope → loudest window → walk BACK to first crossing of 20% of peak. Not the max sample (a click wins that); not the peak (the crack has begun before the peak — aligning there lands audibly late). Cached per buffer. Shared machinery for future footstep-loop segmentation; wet/reverby material defeats it (no quiet gaps) — see session notes on gap-floor classification before segmenting.

## Colour
One authored colour (`mLightningColor`, keyframed): the sheath — ionised nitrogen violet, the colour people mean. The core is blackbody ~30 kK ≈ white regardless; `mLightningCoreWhite` (default 0.85) pulls core from sheath colour toward white, 0 = coloured all the way through (nature never; that's the fantasy dial). Cloud flash uses the sheath colour — the core is the part that doesn't get out of the cloud.
