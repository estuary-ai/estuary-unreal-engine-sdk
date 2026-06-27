# MetaHuman Face Integration Guide — Estuary ARKit-52 Lipsync (Live Link)

This guide shows how to wire Estuary's ARKit-52 lipsync into a MetaHuman's
`Face_AnimBP` so that `bot_animation` frames produce visible, audio-synchronised
mouth motion. **Per-character setup is now a single field**: the Subject Name on
the existing `LiveLinkPose` AnimGraph node.

**Required:** Unreal Engine 5.6+, MetaHuman content set, Estuary plugin built.

---

## Overview

The Estuary plugin delivers ARKit-52 blendshape weights at 30 fps via the
`bot_animation` Socket.IO event. `UEstuaryAnimationReceiver` reads those weights
on every game tick, linearly interpolates between frames (so 60+ fps renders look
smooth), and publishes them through a Live Link source registered by the
`EstuaryLiveLink` runtime module.

The MetaHuman face rig consumes the curves through the stock `LiveLinkPose`
AnimGraph node already present in `Face_AnimBP`. From there, the existing
`mh_arkit_mapping_pose_A2F` PoseBlendNode drives the MetaHuman rig — preserving
all correctives and wrinkle maps.

---

## Prerequisites

- Unreal Engine **5.6+** with the engine-bundled `LiveLink` plugin enabled (the
  Estuary plugin auto-enables it via `Estuary.uplugin`'s `Plugins[]` array — no
  manual step required).
- MetaHuman face set up under `Content/MetaHumans/<Name>/Face/Face_AnimBP`. **No
  reparenting is required**; the stock MetaHuman parent class is correct.
- An `AEstuaryCharacter` (or subclass) in the level with `UEstuaryAnimationReceiver`
  attached (auto-created on the actor by default).

---

## Per-Character Setup (one step)

1. Open the character's `Face_AnimBP` (e.g.,
   `Content/MetaHumans/MetaHuman/Face/Face_AnimBP`).
2. In the AnimGraph, the stock MetaHuman `LiveLinkPose` node already exists; if
   you customised the AnimBP and removed it, drop a **Live Link Pose** node before
   the existing `mh_arkit_mapping_pose_A2F` PoseBlendNode.
3. With the `LiveLinkPose` node selected, set its **Live Link Subject Name** in
   the Details panel to your character's GUID — the same value passed as
   `character_id` in the `authenticate` payload (e.g.,
   `<your-character-id>`).
4. **Compile** and **Save**.

That's it. No Modify Curve node, no Property Access bindings, no 52-curve
clickfest.

---

## Subject Name Override

To disambiguate two character instances that share a CharacterId (e.g., two
copies of the same agent in the same scene), set
`UEstuaryCharacterComponent::LiveLinkSubjectOverride` (an `FName` UPROPERTY,
EditAnywhere) on the second character's component. Resolution order on every
publish:

1. `LiveLinkSubjectOverride` (when not `None`).
2. `CharacterId` (FString → FName).
3. `OwnerActor->GetFName()` (last resort).

Then mirror the override value in that character's `LiveLinkPose` Subject Name.

---

## Custom Face Rigs

For non-MetaHuman ARKit-52 rigs (custom CC4 skins, MHA v1 variants, etc.):

1. Duplicate `Content/DataTables/DT_ArkitRemap_Identity.csv`, rename
   per profile (e.g. `DT_ArkitRemap_MHAv1.csv`).
2. Edit each row's `TargetCurveName` to match your rig's curve naming. Identity
   rows have `TargetCurveName == RowName`; non-identity rows can use any name and
   `ScaleFactor` (e.g., negative to invert axis).
3. In the Content Browser: **Import** → Row Type = `EstuaryArkitRemapRow`.
4. Assign the imported `UDataTable` to `UEstuaryAnimationReceiver::RemapAsset` in
   your character's component Details panel.

The `EstuaryLiveLink` publisher reads the table once per (subject, RemapAsset)
pair on the first frame of an utterance, captures `TargetCurveName` +
`ScaleFactor` per ARKit-52 wire name, and pushes them as the Live Link
`PropertyNames` schema. From then on, per-frame data is just 52 floats — the
remap is zero per-tick cost.

---

## ARKit-52 Curve Reference

The 52 canonical ARKit-52 wire names (matches SDK_CONTRACT
§animation_stream wire names exactly):

| # | Curve Name | # | Curve Name |
|---|------------|---|------------|
| 1 | eyeBlinkLeft | 27 | mouthFrownRight |
| 2 | eyeLookDownLeft | 28 | mouthDimpleLeft |
| 3 | eyeLookInLeft | 29 | mouthDimpleRight |
| 4 | eyeLookOutLeft | 30 | mouthStretchLeft |
| 5 | eyeLookUpLeft | 31 | mouthStretchRight |
| 6 | eyeSquintLeft | 32 | mouthRollLower |
| 7 | eyeWideLeft | 33 | mouthRollUpper |
| 8 | eyeBlinkRight | 34 | mouthShrugLower |
| 9 | eyeLookDownRight | 35 | mouthShrugUpper |
| 10 | eyeLookInRight | 36 | mouthPressLeft |
| 11 | eyeLookOutRight | 37 | mouthPressRight |
| 12 | eyeLookUpRight | 38 | mouthLowerDownLeft |
| 13 | eyeSquintRight | 39 | mouthLowerDownRight |
| 14 | eyeWideRight | 40 | mouthUpperUpLeft |
| 15 | jawForward | 41 | mouthUpperUpRight |
| 16 | jawLeft | 42 | cheekPuff |
| 17 | jawRight | 43 | cheekSquintLeft |
| 18 | jawOpen | 44 | cheekSquintRight |
| 19 | mouthClose | 45 | noseSneerLeft |
| 20 | mouthFunnel | 46 | noseSneerRight |
| 21 | mouthPucker | 47 | browDownLeft |
| 22 | mouthLeft | 48 | browDownRight |
| 23 | mouthRight | 49 | browInnerUp |
| 24 | mouthSmileLeft | 50 | browOuterUpLeft |
| 25 | mouthSmileRight | 51 | browOuterUpRight |
| 26 | mouthFrownLeft | 52 | tongueOut |

---

## Migration from v0.1 (Modify Curve)

If you reparented `Face_AnimBP` to `UEstuaryFaceAnimInstance` or wired a
**Modify Curve** node with 52 Property Access bindings under the v0.1 guide,
follow these steps to migrate:

1. Open `Face_AnimBP`. **Reparent** back to stock `UAnimInstance` (or the
   MetaHuman default Face AnimBP class). Use **Class Settings → Parent Class**.
2. **Delete** the `Modify Curve` node and any 52 Property Access bindings.
3. Drop a **Live Link Pose** node before `mh_arkit_mapping_pose_A2F`.
4. Set the node's **Live Link Subject Name** to your CharacterId.
5. **Compile** and **Save**.

`UEstuaryFaceAnimInstance` remains as a deprecated empty stub for one minor
version (so reparented user blueprints continue to load), then is removed in v2.
The editor surfaces a "DEPRECATED" badge whenever the class appears in a
Blueprint cast or class-defaults picker.

> **UE 5.7 note:** `EModifyCurveApplyMode::Replace` no longer exists in UE 5.7.
> Old guides (NVIDIA ACE docs, our v0.1 guide) reference it; use `Blend` with
> `Alpha=1.0` if you keep a Modify Curve node elsewhere. None of this matters
> for the LiveLink flow — `LiveLinkPose` writes through `Output.Curve` directly.

---

## Verifying It Works

In PIE, after speaking with a character that has a `LiveLinkPose`-wired
`Face_AnimBP`:

- The MetaHuman's mouth/jaw moves in sync with TTS audio.
- **Window → Virtual Production → Live Link** shows a source named **Estuary**
  with status **Active**, and one Subject per active character (Subject names
  match the resolution order above).
- The Output Log contains:
  - `EstuaryLiveLink: ILiveLinkClient acquired and source added.` (once at
    startup, sometimes deferred until after LiveLinkInterface module loads).
  - `[AnimationReceiver] New utterance msg=… anchoring audio offset at X.Xs.`
    (per turn).
- On `client_interrupt`, the face ramps to neutral over ~100 ms — no
  frozen-face snap (Pitfall 25 — LiveLinkPose holds the last-pushed frame
  indefinitely; the receiver's ramp loop publishes ramped frames + one zero
  settle frame to neutralise the cache).

---

## Architecture Reference

```
bot_animation (Socket.IO)
  |
  v
UEstuarySubsystem::HandleBotAnimation
  [filters terminator frames]
  |
  v
UEstuaryCharacterComponent::OnBotAnimation (broadcast)
  |
  v
UEstuaryAnimationReceiver::HandleBotAnimationEvent
  |-> FAnimationFrameBuffer::Push(frame)
  |
  [each TickComponent at game framerate]
  |-> audio clock = UEstuaryAudioPlayerComponent::GetPlaybackTimeSeconds()
  |-> weights = FrameBuffer.GetInterpolatedWeights(RelativeAudioTime)
  |-> FEstuaryLiveLinkBridge::GetPublisher()->PublishFrame(SubjectName,
                                                            weights, WorldTime,
                                                            CurrentMessageId)
         |
         v
  FEstuaryLiveLinkPublisher (in EstuaryLiveLink module)
  |-> Apply RemapAsset (DT_ArkitRemap) — 52 ScaleFactor multiplications
  |-> Clamp [0, 1] per curve
  |-> Push 52 PropertyValues via PushSubjectFrameData_AnyThread
         |
         v
  ULiveLinkBasicRole subject cache (engine, internally locked)
         |
         v
  [LiveLinkPose AnimGraph node, evaluated on anim worker thread]
  |-> Reads PropertyValues for the subject
  |-> Writes Output.Curve.Set(name, value) per pair
         |
         v
  mh_arkit_mapping_pose_A2F → MetaHuman facial rig → visible mouth motion
```

---

*Last updated: 2026-04-27 — Live Link integration (Path B).*
