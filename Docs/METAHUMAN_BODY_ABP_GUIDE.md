# MetaHuman Body Live Link ABP Integration Guide — Estuary GestureSync 52-Bone Body+Hands

> ⚠️ **EXPERIMENTAL / UNFINISHED — forward reference only:** GestureSync body
> animation is an in-development feature. End-to-end it currently renders a
> **T-pose**, and the IK-retargeter and source-skeleton assets it depends on are
> **NOT shipped with the plugin** — the integrator must author them in their own
> project. This guide is provided as a forward reference for that work, not as a
> description of a working out-of-the-box path.

> ⚠ **IKR-refactor note (read first):** the SDK's body
> LiveLink subject name suffix changed from `_Body` to `_Body_GestureSync`, and the
> wiring topology was restructured. The receiver now publishes
> GestureSync-named bones into the `<characterId>_Body_GestureSync` subject, which a
> hidden `GestureSyncSourceMesh` SkeletalMeshComponent on the character consumes
> via a small `ABP_GestureSyncSourceLiveLink` AnimBP. The visible MetaHuman body
> ABP then uses a `Retarget Pose From Mesh` AnimGraph node sourced from that
> hidden component (rather than a `LiveLinkPose Body` node directly). The
> sections below have had `_Body` updated to `_Body_GestureSync` in place, but the
> topology illustrations + AnimGraph wiring instructions still describe the
> *pre-refactor* path. A full topology rewrite is a deferred follow-up.

This guide shows how to wire Estuary's GestureSync-driven body+hands animation into a
MetaHuman's body Animation Blueprint so that `bot_pose` frames produce visible,
audio-synchronised body and finger motion. **Per-character setup is a single
field**: the Subject Name on a `LiveLinkPose Body` AnimGraph node.

**Required:** Unreal Engine 5.6+, MetaHuman content
set, Estuary plugin built.

This guide is the body counterpart to
[METAHUMAN_ABP_GUIDE.md](METAHUMAN_ABP_GUIDE.md) (face). Read that guide first if
you have not already wired the face Live Link path — many concepts mirror.

---

## 1. Overview

The Estuary plugin delivers GestureSync 52-bone (`22 body + 30 finger`) transforms at
30 fps via the `bot_pose` Socket.IO event. `UEstuaryBodyAnimationReceiver` reads
those transforms on every game tick, linearly interpolates between frames against
the audio clock (so 60+ fps renders stay smooth), applies the SMPL-X→UE coordinate
conversion (`FEstuaryBodyFrameTransform::ToUEFromGestureSync`), and publishes them
through `FEstuaryBodyLiveLinkPublisher` — a Live Link source registered by the
`EstuaryLiveLink` runtime module using `ULiveLinkAnimationRole` (transforms, not
curves like the face).

The MetaHuman body rig consumes the transforms through a `LiveLinkPose Body`
AnimGraph node placed in the body Animation Blueprint. From there, an
`IK Retargeter` AnimNode fed by an `IKR_GestureSyncToMetaHuman` asset (which the
integrator authors — it is NOT shipped with the plugin) handles the
cross-skeleton retarget: GestureSync 22-body chains → MetaHuman 70+-bone body,
including the 1-spine→5-spine subdivision via per-chain Slerp weights ≈ `0.10 /
0.20 / 0.25 / 0.25 / 0.20`. The 30 finger bones BYPASS the IK Retargeter (1:1
with MetaHuman finger bones) and write directly to the rig's finger bones via
the `LiveLinkPose Body` node. Twist + helper bones are intentionally unmapped in
the IKR — the existing MetaHuman post-process Animation BP drives them, **DO NOT
REPLACE IT**.

**Single-step per-character setup ideal:** drop in a `LiveLinkPose Body`
AnimGraph node, set its **Subject Name** to `<characterId>_Body_GestureSync`, compile + save.
That is the entire wiring step; the IKR + post-process chain is authored once by
the integrator (these assets are NOT shipped with the plugin) and reused across
all MetaHuman characters.

---

## 2. Prerequisites

- Unreal Engine **5.6+** with the
  engine-bundled `LiveLink`,
  `LiveLinkAnimationCore`, and `IKRig` plugins enabled (the Estuary plugin
  auto-enables them via `Estuary.uplugin`'s `Plugins[]` array — no manual step
  required).
- MetaHuman content set imported in the project.
- Estuary plugin built, providing:
  - `DT_GestureSyncBoneRemap_Identity.csv` — 52-row SMPL-X → MetaHuman
    name + per-bone roll-offset DataTable.
  - `FEstuaryBodyFrameTransform` math helper — pure
    SMPL-X→UE coordinate conversion (`ToUEQuatFromGestureSync`,
    `ToUEHipsFromGestureSync`).
  - `metahuman_common_v1.json` — bundled bind pose at
    `Content/BindPoses/metahuman_common_v1.json` (52 quats at identity).
- An `IKR_GestureSyncToMetaHuman` IK Retargeter — a 22-chain retargeter
  targeting the MetaHuman skeleton. This asset is **NOT shipped with the
  plugin**; the integrator authors it in their own project (see Section 5).
- An `AEstuaryCharacter` (or subclass) in the level with
  `UEstuaryBodyAnimationReceiver` attached (auto-created on the actor by
  default; verify in `EstuaryCharacter.cpp` ctor).
- Auth payload sets `bEnableBodyAnimation = true` (or project-wide via
  `Project Settings > Plugins > Estuary > bEnableBodyAnimation = true`).

> **NOTE:** Wire quaternions are **LOCAL parent-relative**. The
> `LiveLinkPose Body` AnimGraph node consumes them as bone-local-space rotations
> applied on top of the rig's bind pose. **No global-to-local conversion is
> needed in UE.** The 6-pose math fixture
> (`Source/Estuary/Private/Tests/Fixtures/coord_roundtrip_v1.json`) is authored
> as local quaternions; this convention is hard-gated with sub-1e-4 quat error
> per bone. (Pitfall 7.)

---

## 3. Per-Character Setup (one step)

1. Open the MetaHuman's body Animation Blueprint (e.g.,
   `Content/MetaHumans/MetaHuman/Body/ABP_Body`).
2. In the AnimGraph, drop a **Live Link Pose** node configured for body
   transforms (the engine-bundled `LiveLinkPose Body` variant — pick the role
   that consumes `ULiveLinkAnimationRole`, which is the one with transform
   inputs, not curve inputs).
3. With the `LiveLinkPose Body` node selected, set its **Live Link Subject
   Name** in the Details panel to `<characterId>_Body_GestureSync` — the same value passed
   as `character_id` in the `authenticate` payload, suffixed with `_Body_GestureSync`. For
   example, if your CharacterId is `<your-character-id>`, the
   Subject Name is `<your-character-id>_Body_GestureSync`.
4. Connect the node's pose output to the `IK Retargeter` AnimNode wired in
   Section 5 (IKRetargeter Setup), then through to the rest of the AnimGraph as
   it stands today (Control Rig + post-process ABP downstream — DO NOT
   REPLACE IT).
5. **Compile** and **Save**.

That's it. The body wiring is structurally a mirror of the face's "set Subject
Name" single-step — the only difference is the `_Body_GestureSync` suffix and the IK
Retargeter Setup section that follows.

> **CRITICAL:** Wire quaternions are **LOCAL parent-relative**. The
> `LiveLinkPose Body` AnimGraph node consumes them as bone-local-space rotations
> applied on top of the rig's bind pose. NO global-to-local conversion is
> needed in UE. The 6-pose math fixture
> (`Source/Estuary/Private/Tests/Fixtures/coord_roundtrip_v1.json`) is authored
> as local quaternions. The 52-quat bind pose lives at
> `Content/BindPoses/metahuman_common_v1.json` and is loaded by ID at
> session connect; see Section 9 for the drift-defense note.

---

## 4. Subject Name Override

To disambiguate two character instances that share a CharacterId (e.g., two
copies of the same agent in the same scene), set
`UEstuaryCharacterComponent::LiveLinkBodySubjectOverride` (an `FName`
UPROPERTY, `EditAnywhere`) on the second character's component. This is the
body-side parallel to the face's `LiveLinkSubjectOverride`. Resolution order on
every publish (mirror of face's chain, plus the `_Body_GestureSync` suffix):

1. `LiveLinkBodySubjectOverride` (when not `None`) — used verbatim. Operators
   who set this take responsibility for including the `_Body_GestureSync` suffix manually
   if their downstream `LiveLinkPose Body` node expects it.
2. `CharacterId` (FString → FName) **+ `_Body_GestureSync`** suffix appended automatically
   by `UEstuaryBodyAnimationReceiver::ResolveSubjectName()`.
3. `OwnerActor->GetFName()` (last resort) **+ `_Body_GestureSync`** suffix.

The receiver's `ResolveSubjectName()` helper is the single point of enforcement;
the bare-id from `UEstuaryCharacterComponent::GetLiveLinkSubjectName()` gets the
`_Body_GestureSync` suffix appended automatically. Operators rarely need to override.

Then mirror the resolved Subject Name in that character's `LiveLinkPose Body`
node's Subject Name field.

> **NOTE:** Face stays bare-id (`<characterId>`) to avoid breaking v1.0 face
> ABPs; body MUST suffix with `_Body_GestureSync` to coexist on the same actor without
> subject collision.

---

## 5. IK Retargeter Setup (NEW — body-only)

This section is unique to the body guide; the face guide has no analogue. The
`IKR_GestureSyncToMetaHuman` IK Retargeter is **NOT shipped with the plugin** —
you must author it in your own project. Once authored, its 22-chain mapping +
spine subdivision Slerp weights are baked into the asset, so operators do not
need to set them per-character at runtime.

### 5.1 Wiring the IK Retargeter into the AnimGraph

1. In the MetaHuman body ABP AnimGraph, immediately AFTER the
   `LiveLinkPose Body` node (Section 3), drop in an **IK Retargeter** AnimNode.
2. Set its `IKRetargeter` property (Details panel) to the
   `IKR_GestureSyncToMetaHuman` asset you authored (see the note above — this
   asset is NOT shipped with the plugin). Pick it from the Content Browser path
   picker.
3. Source pose = output of `LiveLinkPose Body` node (pose pin).
4. Target pose = MetaHuman skeletal mesh (the AnimGraph's existing skeleton
   target).
5. Output of the IK Retargeter feeds into the rest of the AnimGraph as it
   stands today — Control Rig and the post-process ABP run downstream.

[Screenshot: AnimGraph layout — `LiveLinkPose Body` → `IK Retargeter` (with
`IKR_GestureSyncToMetaHuman` assigned) → existing Control Rig → existing post-process
ABP → Output Pose]

### 5.2 Spine subdivision (1→5)

The `IKR_GestureSyncToMetaHuman` asset's spine chain maps GestureSync's three spine joints
(`spine` / `chest` / `upperChest`) onto MetaHuman's five spine joints
(`spine_01..05`) with per-target-bone Slerp weights of approximately
**`0.10 / 0.20 / 0.25 / 0.25 / 0.20`**. These weights are
authored into the IKRetargeter asset; you do not set them in the ABP.

### 5.3 Body bones vs. finger bones (split path)

- **Body bones (indices 0..21):** route through the IK Retargeter. The 22-chain
  mapping handles cross-skeleton retargeting (hips, leg ×2, spine 1-5,
  shoulder/arm ×2, neck, head).
- **Finger bones (indices 22..51):** **bypass the IK Retargeter.** GestureSync's 30
  finger bones (5 × 3 phalanges × 2 hands) are 1:1 with MetaHuman's `*_01_l`,
  `*_02_l`, `*_03_l` and right-hand counterparts — using IKRetargeter for
  fingers adds complexity without correctness benefit. The `LiveLinkPose Body`
  node writes finger transforms directly to the rig's finger bones, going
  through `DT_GestureSyncBoneRemap_Identity.csv` lookup +
  `FEstuaryBodyFrameTransform::ToUEFromGestureSync` math helper (handled inside the
  receiver before publish — operators see no extra node in the AnimGraph).

### 5.4 Twist bones, helper bones, metacarpals — DO NOT REPLACE

Twist bones (`upperarm_twist_*`, `calf_twist_*`, etc.), helper bones (clavicle
helpers, MetaHuman finger metacarpals — `index_metacarpal_l`,
`middle_metacarpal_l`, `ring_metacarpal_l`, `pinky_metacarpal_l`, plus the
right-hand counterparts), and shoulder corrective bones are **intentionally
unmapped in the `IKR_GestureSyncToMetaHuman` asset**. They are driven by the existing
MetaHuman post-process Animation BP (**DO NOT REPLACE IT**). The
post-process ABP infers twist + metacarpal rotation from the finger 1+2+3
rotations the receiver writes (Pitfall 11 mitigation: SMPL-X has no
metacarpals; MetaHuman does; the post-process ABP closes the topology gap).

[Screenshot: IK Retargeter editor — chain list showing 22 source chains
(GestureSync 22-body) mapped to MetaHuman target chains; twist + helper bones absent
(intentional)]

### 5.5 PIE save caveat

Always exit PIE before saving the `IKR_GestureSyncToMetaHuman.uasset` (or any
IKRetargeter asset). PIE locks asset saves; see Section 10 for the full caveat.

---

## 6. FingerPolicy Configuration (NEW — body-only)

The receiver exposes a `FingerPolicy` UPROPERTY
(`UEstuaryBodyAnimationReceiver::FingerPolicy`) with four enum values.
These control how finger bones are driven during normal playback,
pre-buffer windows, and gaps in the frame stream.

| Policy | Behavior | When to use |
|--------|----------|-------------|
| `Driven` (default) | Live GestureSync frames drive fingers when buffer has frames. Bind-pose hold during 200 ms pre-buffer or any gap > 100 ms. | Default for all production characters (v2.0). |
| `BindPose` | Always rest pose; fingers never move. | Kiosk demos where finger animation is undesired. Debug fallback when investigating finger-only artifacts. |
| `IdleBreath` | `Driven` plus subtle additive idle motion overlaid. | v2.x ergonomics work — ships in v2.0 as the same behaviour as `Driven` (the additive idle math is a stub; additive finger idle is not yet implemented). |
| `Statue` | Frozen at last received frame indefinitely. | Debug only — surfaces "frames stopped flowing but client thought they were live" as a visible artifact. **Never the default.** |

Project-wide default settable via
**Project Settings > Plugins > Estuary > Animation|Body > Default Finger Policy**
(this UPROPERTY lives on `UEstuaryDeveloperSettings`; per-receiver
UPROPERTY overrides this in the Details panel).

> **NOTE:** During the 200 ms pre-buffer window (before the first frame
> arrives), behavior is **bind-pose hold** under `Driven`, `BindPose`, or
> `IdleBreath`. This mirrors the face receiver's pre-buffer behavior:
> LiveLinkPose neutral cache until first frame published. `Statue` is a debug
> indicator and should not be left in a shipping build.

---

## 7. Custom Body Rigs

For non-MetaHuman skeletons (custom CC4 / Mixamo / hand-rigged bodies):

1. **Duplicate the bone remap CSV.** Copy
   `Content/DataTables/DT_GestureSyncBoneRemap_Identity.csv` to a new file
   named per profile (e.g., `DT_GestureSyncBoneRemap_CC4.csv`).
2. **Edit row mappings.** For each row, edit `UEBoneName` to match your rig's
   bone naming (the SMPL-X canonical name in the `RowName` column stays fixed).
   Edit `RollOffsetDeg` for any bone where your rig has a baked-in twist
   relative to its SMPL-X counterpart (most rows stay at 0.0; populate
   empirically when the 6-pose test diagnoses Roll mode for a specific bone).
3. **Re-import as DataTable.** In the Content Browser:
   **Import** → Row Type = `EstuaryBodyBoneRemapRow`.
4. **Assign to receiver.** Set the imported `UDataTable` on
   `UEstuaryBodyAnimationReceiver::BoneRemapAsset` in your character's
   component Details panel.
5. **Author a custom IK Retargeter.** Open `IKR_GestureSyncToMetaHuman.uasset`,
   **Duplicate** it (e.g., `IKR_GestureSyncToCC4.uasset`), and re-target the chains
   onto your custom skeleton. Re-author the spine subdivision Slerp weights if
   your rig has a different spine joint count (the `0.10 / 0.20 / 0.25 / 0.25 /
   0.20` weights are MetaHuman-specific).

The plain DataTable + IKR seam IS the per-rig customization point. For rigs
without metacarpals (most CC4, Mixamo), the 1:1 finger mapping is correct
without further work.

---

## 8. 52-Bone SMPL-X Reference Table

The 52 canonical SMPL-X wire bone names + MetaHuman target bone names match
`SDK_CONTRACT.md §body_animation_stream` — the backend body model's canonical
52-bone order. The bundled `DT_GestureSyncBoneRemap_Identity.csv` DataTable
encodes this same mapping.

### Body 22 (indices 0..21)

| # | SMPL-X Name | MetaHuman Bone |
|---|-------------|----------------|
| 1 | hips | pelvis |
| 2 | leftUpperLeg | thigh_l |
| 3 | rightUpperLeg | thigh_r |
| 4 | spine | spine_01 |
| 5 | leftLowerLeg | calf_l |
| 6 | rightLowerLeg | calf_r |
| 7 | chest | spine_03 |
| 8 | leftFoot | foot_l |
| 9 | rightFoot | foot_r |
| 10 | upperChest | spine_05 |
| 11 | leftToes | ball_l |
| 12 | rightToes | ball_r |
| 13 | neck | neck_01 |
| 14 | leftShoulder | clavicle_l |
| 15 | rightShoulder | clavicle_r |
| 16 | head | head |
| 17 | leftUpperArm | upperarm_l |
| 18 | rightUpperArm | upperarm_r |
| 19 | leftLowerArm | lowerarm_l |
| 20 | rightLowerArm | lowerarm_r |
| 21 | leftHand | hand_l |
| 22 | rightHand | hand_r |

### Left fingers 15 (indices 22..36)

| # | SMPL-X Name | MetaHuman Bone |
|---|-------------|----------------|
| 23 | leftIndex1 | index_01_l |
| 24 | leftIndex2 | index_02_l |
| 25 | leftIndex3 | index_03_l |
| 26 | leftMiddle1 | middle_01_l |
| 27 | leftMiddle2 | middle_02_l |
| 28 | leftMiddle3 | middle_03_l |
| 29 | leftPinky1 | pinky_01_l |
| 30 | leftPinky2 | pinky_02_l |
| 31 | leftPinky3 | pinky_03_l |
| 32 | leftRing1 | ring_01_l |
| 33 | leftRing2 | ring_02_l |
| 34 | leftRing3 | ring_03_l |
| 35 | leftThumb1 | thumb_01_l |
| 36 | leftThumb2 | thumb_02_l |
| 37 | leftThumb3 | thumb_03_l |

### Right fingers 15 (indices 37..51)

| # | SMPL-X Name | MetaHuman Bone |
|---|-------------|----------------|
| 38 | rightIndex1 | index_01_r |
| 39 | rightIndex2 | index_02_r |
| 40 | rightIndex3 | index_03_r |
| 41 | rightMiddle1 | middle_01_r |
| 42 | rightMiddle2 | middle_02_r |
| 43 | rightMiddle3 | middle_03_r |
| 44 | rightPinky1 | pinky_01_r |
| 45 | rightPinky2 | pinky_02_r |
| 46 | rightPinky3 | pinky_03_r |
| 47 | rightRing1 | ring_01_r |
| 48 | rightRing2 | ring_02_r |
| 49 | rightRing3 | ring_03_r |
| 50 | rightThumb1 | thumb_01_r |
| 51 | rightThumb2 | thumb_02_r |
| 52 | rightThumb3 | thumb_03_r |

> **NOTE:** MetaHuman metacarpals (`index_metacarpal_l`, `middle_metacarpal_l`,
> `ring_metacarpal_l`, `pinky_metacarpal_l`, plus right-hand counterparts) are
> **driven by the existing post-process ABP**; do NOT manually map them in
> `DT_GestureSyncBoneRemap`. SMPL-X does not include metacarpals; the post-process
> ABP infers their rotation from the finger 1+2+3 phalanges. (Pitfall 11.)

---

## 9. Bind-Pose Drift Note (NEW — body-only / Pitfall 5 mitigation)

The plugin's bind pose lives at
`Content/BindPoses/metahuman_common_v1.json` (52 quats at identity),
loaded client-side at session connect. It is the UE-side copy of the same
52-quat reference pose the backend body model retargets against.

**The two MUST stay in sync.** When the backend's reference bind pose changes,
update the plugin copy to match in the same change so the wire stays consistent
end-to-end.

> **CRITICAL:** If the plugin bind pose and the backend's reference pose drift
> apart, the backend retargets against one bind pose while UE assumes another.
> The mismatch produces a small but consistent rotation offset on every bone —
> visually "everything is slightly off" but no test fails because the wire is
> technically correct. **The 6-pose math gate cannot catch this** — the math is
> correct against the wire; only the bind pose itself is wrong. Visual smoke
> (T-pose match) is the only detector.

---

## 10. PIE Save Caveat (NEW — body-only / Pitfall 10 mitigation)

**Always exit PIE before saving an IKRetargeter** (or any UE asset that lives
in plugin Content). PIE locks asset saves; the editor may show "asset saved"
but on next session the changes are absent. Asset-save APIs return failure
during PIE.

After save, **tail the Output Log** for save-failure errors:

- `Illegal reference to private object: AssetImportData` (silent save failures
  are common; they lead to in-memory-only changes that evaporate on editor
  close).
- `Failed to save package` warnings.
- `LogContentBrowser: Warning: Asset is dirty after save`.

If you see any of these, **re-load the asset** (Right-click in Content Browser
→ Reimport, or close + reopen the editor + re-check the asset's modified
timestamp) before proceeding. A "saved" badge in the editor is not enough
proof of persistence.

> **NOTE:** This caveat applies to **every** plugin asset edit during a PIE
> session, not just IKRetargeters. The face guide does not call this out
> because the face wiring is a single Subject Name field on a stock node — no
> plugin-Content asset edits required. Body wiring touches `IKR_*.uasset` and
> `DT_GestureSyncBoneRemap_*.csv` (re-import as DataTable), so the caveat is
> body-only.

---

## Verifying It Works

In PIE, after speaking with a character that has a `LiveLinkPose Body`-wired
body ABP + IKR + post-process ABP downstream, AND
`bEnableBodyAnimation = true` in the auth payload:

- The MetaHuman's body and fingers move in sync with TTS audio.
- **Window → Virtual Production → Live Link** shows a source named **Estuary**
  with status **Active**, and one Subject per active character with the
  `_Body_GestureSync` suffix (e.g., `<your-character-id>_Body_GestureSync`).
- The Output Log contains:
  - `EstuaryLiveLink: ILiveLinkClient acquired and source added.` (once at
    startup, sometimes deferred until after `LiveLinkInterface` module loads —
    Pitfall 24).
  - `[BodyAnimationReceiver] New utterance msg=… anchoring audio offset at X.Xs.`
    (per turn).
- On `client_interrupt`, the body ramps to bind pose over **~500 ms** (body
  uses 500 ms because body motion has more inertia than face's 100 ms),
  then publishes a settle frame so `LiveLinkPose Body` neutralises (Pitfall
  25 — LiveLinkPose holds the last-pushed frame indefinitely; the receiver's
  ramp loop publishes ramped frames + one settle frame to neutralise the
  cache).
- Cross-modality drift between face and body is **sub-100 ms by construction**
  — both receivers read the SAME
  `WeakAudioComp->GetPlaybackTimeSeconds()` per tick.

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|--------------|-----|
| Subject not visible in Live Link panel | Subject Name mismatch between `LiveLinkPose Body` node and receiver-resolved name | Verify CharacterId matches; check for `_Body_GestureSync` suffix on the AnimGraph node |
| Body explodes outward from origin on first frame | Quaternions interpreted as world-space, not local-relative | Pitfall 7 — wire is LOCAL parent-relative; verify no global-to-local conversion code path |
| Pose looks "almost right" with consistent small offset on every bone | Bind-pose drift between the plugin copy and the backend's reference pose | Pitfall 5 — re-sync the plugin bind pose per Section 9 |
| Frozen pose mid-utterance after interrupt | LiveLinkPose stale-frame cache; ramp loop missing or settle frame not pushed | Pitfall 25 — verify `kBodyInterruptRampSecs = 0.5f` ramps + final all-zero settle frame |
| Fingers articulate but base looks pinned to wrist with kink | MetaHuman metacarpals at bind pose; post-process ABP not driving them | Pitfall 11 — DO NOT REPLACE the post-process ABP; metacarpals are inferred from finger 1+2+3 |
| Wrong roll on a specific bone (otherwise correct) | Per-bone roll offset missing in `DT_GestureSyncBoneRemap_*.csv` | Set `RollOffsetDeg` for that bone empirically (degrees) using 6-pose test residual |
| 6-pose math test passes, visual still wrong on T-pose | Bind pose drift OR IKR chain weights mis-authored | Section 9 (drift) or re-check the IKR chain authoring (Section 5) |

For detailed failure-mode diagnosis, the receiver writes
`[BodyAnimationReceiver] Diagnose: residual matches mode=AxisSwap|SignFlip|Handedness|Roll`
to the Output Log on math-fixture mismatches (the 4 SMPL-X→UE conversion
failure modes named explicitly).

---

## Architecture Reference

```
bot_pose (Socket.IO)
  |
  v
UEstuarySubsystem::HandleBotPose
  [filters terminator frames]
  |
  v
UEstuaryCharacterComponent::OnBotPose (broadcast)
  |
  v
UEstuaryBodyAnimationReceiver::HandleBotPoseEvent
  |-> FEstuaryAnimationFrameBuffer<FEstuaryBotPose>::Push(frame)
  |
  [each TickComponent at game framerate]
  |-> audio clock = UEstuaryAudioPlayerComponent::GetPlaybackTimeSeconds()
  |-> FEstuaryBotPose pose = FrameBuffer.GetInterpolatedPose(RelativeAudioTime)
  |-> for each bone: FEstuaryBodyFrameTransform::ToUEFromGestureSync(pose.Bones[i].LocalQuat,
                                                                 pose.HipsLocalToFloor,
                                                                 RollOffsetDeg)
  |-> apply DT_GestureSyncBoneRemap_Identity bone-name lookup
  |-> FEstuaryLiveLinkBridge::GetBodyPublisher()->PublishFrame(SubjectName,
                                                                Transforms[52],
                                                                WorldTime,
                                                                CurrentMessageId)
         |
         v
  FEstuaryBodyLiveLinkPublisher (in EstuaryLiveLink module)
  |-> Push 52 FTransforms via PushSubjectFrameData_AnyThread (ULiveLinkAnimationRole)
  |-> Static data: FLiveLinkSkeletonStaticData (52 BoneNames + ParentIndices)
         |
         v
  ULiveLinkAnimationRole subject cache (engine, internally locked)
         |
         v
  [LiveLinkPose Body AnimGraph node, evaluated on anim worker thread]
  |-> Reads Transforms[52] for the subject
  |-> Outputs Source pose
         |
         v
  [IK Retargeter AnimNode — IKR_GestureSyncToMetaHuman.uasset]
  |-> 22-chain retarget (body bones; spine 1→5 Slerp 0.10/0.20/0.25/0.25/0.20)
  |-> Fingers (22..51) bypass IKR (1:1 mapping)
         |
         v
  [Existing MetaHuman post-process ABP — DO NOT REPLACE]
  |-> Drives twist bones, helper bones, metacarpals
         |
         v
  Output Pose → MetaHuman body rig → visible body + finger motion
```

Cross-references:
- Mirror of [METAHUMAN_ABP_GUIDE.md](METAHUMAN_ABP_GUIDE.md) (face).
- Sample BP walkthrough: see
  [`Docs/Samples/BP_EstuaryMetaHumanBodySample.md`](Samples/BP_EstuaryMetaHumanBodySample.md)
  for a complete actor + ABP wiring tour.

---

*Last updated: 2026-05-01 — body Live Link integration guide.*
