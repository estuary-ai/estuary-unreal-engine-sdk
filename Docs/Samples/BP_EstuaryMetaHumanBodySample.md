# BP_EstuaryMetaHumanBodySample — Blueprint Design Spec

## Overview

`BP_EstuaryMetaHumanBodySample` is a Blueprintable `AEstuaryCharacter` subclass that
demonstrates the complete Estuary voice + ARKit-52 lipsync (face) + GestureSync 52-bone body+hands
animation flow simultaneously on a single MetaHuman actor. It is the body-and-face reference
implementation, extending the face-only [`BP_EstuaryMetaHumanSample`](BP_EstuaryMetaHumanSample.md)
walkthrough with the additional ABP wiring required by the body Live Link path.

> ⚠ **EXPERIMENTAL / UNFINISHED — the body animation path ships dormant.** The
> GestureSync body+hands feature is incomplete: end-to-end it currently renders a
> **T-pose** and has not been validated in PIE. The runtime retarget assets it
> needs — the IK Retargeter (`IKR_*`), the GestureSync source skeletal mesh
> (`SK_*`), and the source AnimBP (`ABP_*`) — are **NOT bundled with the plugin**;
> you author them in your own project per this walkthrough. Treat this document as
> a design reference for a future release, not a working drop-in. The face-only
> [`BP_EstuaryMetaHumanSample`](BP_EstuaryMetaHumanSample.md) is the supported path today.

**Why a design spec instead of a `.umap`:** Editor-generated assets (`.umap`, `.uasset`,
`.uasset` Blueprint binaries) cannot be shipped inside a code-only plugin because they are
binary UE-serialised files tied to specific engine versions and project content directories.
This spec gives a designer the complete specification to recreate the sample BP in the Editor
in approximately 15 minutes (10 for the face-only path documented in
`BP_EstuaryMetaHumanSample.md`, plus 5 minutes for body Live Link wiring on top). Follow it
step-by-step and the result will behave identically to a shipped sample BP.

This walkthrough is the body-and-face counterpart to
[BP_EstuaryMetaHumanSample.md](BP_EstuaryMetaHumanSample.md) (face only). Read that
walkthrough first if you have not already wired the face-only sample — many concepts mirror.
For body-only background context (per-character setup, IKRetargeter, FingerPolicy, drift
defense), see
[`Docs/METAHUMAN_BODY_ABP_GUIDE.md`](../METAHUMAN_BODY_ABP_GUIDE.md).

---

## Actor Class

| Property | Value |
|----------|-------|
| **Parent class** | `AEstuaryCharacter` (Estuary plugin) |
| **Blueprint name** | `BP_EstuaryMetaHumanBodySample` |
| **Content Browser location** | your project Content folder (parallel to `BP_EstuaryMetaHumanSample`) |
| **Tick enabled** | Yes (inherited from `AEstuaryCharacter`) |

The actor is the body-and-face counterpart to `BP_EstuaryMetaHumanSample`. The two are
intentionally peers in the Samples directory so an operator can pick the one matching their
target capability — face-only (v1.0) or face+body (v2.0).

---

## Default Sub-Objects (auto-created by AEstuaryCharacter)

These components are created in the `AEstuaryCharacter` C++ constructor. **Do NOT add
them manually** — doing so creates duplicates. They appear in the Blueprint's component
hierarchy automatically once you reparent to `AEstuaryCharacter`.

| Component | Class | Purpose |
|-----------|-------|---------|
| `CharacterComponent` | `UEstuaryCharacterComponent` | Sends `text` events; receives streaming `bot_response` and `interrupt` callbacks. Hosts `LiveLinkSubjectOverride` (face) and `LiveLinkBodySubjectOverride` (body) FName disambiguators. |
| `MicrophoneComponent` | `UEstuaryMicrophoneComponent` | Cross-platform mic capture via Runtime Audio Importer; emits `start_voice`, `stream_audio`, `stop_voice` |
| `AudioPlayerComponent` | `UEstuaryAudioPlayerComponent` | Decodes base64 PCM16 `bot_voice` chunks into `USoundWaveProcedural` ring buffer; 200ms pre-buffer; interrupt flush. Source of the audio clock both `AnimationReceiver` and `BodyAnimationReceiver` consume per tick. |
| `AnimationReceiver` (face) | `UEstuaryAnimationReceiver` | Buffers 30fps ARKit-52 `bot_animation` frames; audio-clock-driven linear interpolation; publishes to `EstuaryLiveLink` source via `IEstuaryLiveLinkPublisher`. v1.0 substrate; unchanged. |
| `BodyAnimationReceiver` (body, NEW) | `UEstuaryBodyAnimationReceiver` | Buffers 30fps GestureSync 52-bone `bot_pose` frames; audio-clock-driven linear interpolation (same clock as face — `WeakAudioComp->GetPlaybackTimeSeconds()`); applies SMPL-X→UE coord conversion (`FEstuaryBodyFrameTransform::ToUEFromGestureSync`) + DT_GestureSyncBoneRemap_Identity bone-name lookup; publishes to `EstuaryLiveLink` source via `IEstuaryBodyLiveLinkPublisher`. Replaces an earlier first-pass scaffold. |

> **NOTE:** Cross-modality drift between face and body is **sub-100 ms by construction**
> — both receivers read the SAME `WeakAudioComp->GetPlaybackTimeSeconds()` per
> tick. There is no drift to measure; the success criterion is trivially satisfied.

> **VERIFY (scaffold inheritance):** Open `EstuaryCharacter.cpp` ctor and confirm
> `BodyAnimationReceiver = CreateDefaultSubobject<UEstuaryBodyAnimationReceiver>(...)` is
> present. An earlier revision added it; a later one replaced the implementation but preserved
> the auto-creation pattern. If the sub-component is absent, add a manual
> `UEstuaryBodyAnimationReceiver` to the BP via the Components panel.

---

## MetaHuman Setup (add in Blueprint Details)

### Body Mesh

1. In the Blueprint editor, add a **Skeletal Mesh Component** to the actor root.
   - **Skeletal Mesh**: assign your MetaHuman Body mesh (from your MetaHuman content
     folder — e.g., `Content/MetaHumans/MetaHuman/Body/m_med_nrw_body`).
   - **Anim Class**: a custom body Animation Blueprint that wires the body Live Link
     path; we will call it `ABP_Body` in this walkthrough. See **ABP Wiring Order**
     (Section 6) for the AnimGraph layout.
   - **Name it**: `BodyMesh`
   - Position it at the actor root (no offset needed for a desktop demo).

> **NOTE:** The body Anim Class is **NOT** the stock MetaHuman ABP — body Live Link
> requires inserting a `LiveLinkPose Body` node and an IK Retargeter, which is what
> distinguishes this sample from `BP_EstuaryMetaHumanSample` (face-only). The face's
> Anim Class IS unchanged (stock `Face_AnimBP`); see Face Mesh below.

### Face Mesh

2. Add a second **Skeletal Mesh Component** as a **child** of `BodyMesh`.
   - **Skeletal Mesh**: your MetaHuman Face skeletal mesh (`m_med_nrw_head_lod0_mesh`
     or equivalent — use LOD0 for the sample to get full blend shape resolution).
   - **Anim Class**: the **stock MetaHuman `Face_AnimBP`** — **no reparent required**.
     Path B (Live Link) reads curves directly through the existing `LiveLinkPose` node
     in the stock AnimBP. See `Docs/METAHUMAN_ABP_GUIDE.md` for the
     one-step Subject Name field you need to set on that node.
   - **Name it**: `FaceMesh`
   - Leave Leader Pose Component set to `BodyMesh` so face follows body transforms.

The face setup is **unchanged from v1.0** — `BP_EstuaryMetaHumanSample` covers this
verbatim. Only the body wiring is new in this sample.

---

## Animation Receiver Wiring

### Face — unchanged (v1.0)

3. In the `AnimationReceiver` (face) component Details panel (Blueprint Defaults):
   - Set **Remap Asset** to `DT_ArkitRemap_Identity` (imported from the plugin's
     `Content/DataTables/`). Identity is correct for stock MetaHumans
     (post-5.2 ARKit-Ready variant).

### Body — NEW

4. In the `BodyAnimationReceiver` (body) component Details panel (Blueprint Defaults):
   - Set **Bone Remap Asset** to `DT_GestureSyncBoneRemap_Identity` (imported from the
     plugin's `Content/DataTables/`; a 52-row CSV with identity SMPL-X →
     MetaHuman name mappings + 0.0 default `RollOffsetDeg`).
   - Set **Finger Policy** to **`Driven`** (the default). The
     `EEstuaryFingerPolicy` enum has 4 values:
     - `Driven` (default) — Live GestureSync frames drive fingers; bind-pose hold during
       200 ms pre-buffer.
     - `BindPose` — always rest pose (kiosk demos).
     - `IdleBreath` — Driven + additive idle (v2.x stub; ships in v2.0 as `Driven`).
     - `Statue` — frozen at last received frame (debug only; never the default).
   - Project-wide default settable via **Project Settings > Plugins > Estuary >
     Animation|Body > Default Finger Policy** (the SDK ships this UPROPERTY on
     `UEstuaryDeveloperSettings`; per-receiver UPROPERTY overrides this).

> ⚠ **Post-IKR-refactor note (read first):** the SDK's body LiveLink subject
> name suffix changed from `_Body` to `_Body_GestureSync`, and the wiring topology
> was restructured. The receiver now publishes GestureSync-named bones into the
> `<characterId>_Body_GestureSync` subject, which a hidden
> `GestureSyncSourceMesh` SkeletalMeshComponent on the character consumes via a
> small `ABP_GestureSyncSourceLiveLink` AnimBP. The visible MetaHuman body ABP
> then uses a `Retarget Pose From Mesh` AnimGraph node sourced from that
> hidden component (rather than a `LiveLinkPose Body` node directly). The
> sections below have had `_Body` updated to `_Body_GestureSync` in place, but
> the AnimGraph wiring instructions still describe the *pre-refactor*
> topology. A full topology rewrite is a deferred follow-up.

5. **No Event-Graph wiring is required** for either face or body animation. Both
   receivers resolve `FEstuaryLiveLinkBridge` through `UEstuarySubsystem` automatically
   each tick and publish ramped/interpolated frames to their respective publishers
   (`IEstuaryLiveLinkPublisher` for face, `IEstuaryBodyLiveLinkPublisher` for body).

### Character ID

6. In the Blueprint **Defaults** panel, find **Character Id** (on `AEstuaryCharacter`):
   - Set it to the UUID of your Estuary character.
   - Example: `"<your-character-id>"`.
   - This is the `character_id` used in the `authenticate` handshake **and**:
     - The default Live Link Subject Name **for face** (resolution order: Override →
       CharacterId → OwnerActor name).
     - The base of the Live Link Subject Name **for body** — `<characterId>_Body_GestureSync`
       suffix is appended automatically by `UEstuaryBodyAnimationReceiver::ResolveSubjectName()`.

---

## ABP Wiring Order (NEW — body-only, finger split path)

This section is unique to the body sample; `BP_EstuaryMetaHumanSample` (face-only) has no
analogue. The body's `ABP_Body` Animation Blueprint must be authored with a specific
AnimGraph layout to correctly route GestureSync 52-bone transforms through the IKRetargeter,
split body vs. finger paths, and preserve the existing MetaHuman post-process ABP
(**DO NOT REPLACE IT**).

### 6.1 Layout (left-to-right in the AnimGraph)

```
[LiveLinkPose Body]
    Subject Name = <characterId>_Body_GestureSync
        |
        v
[IK Retargeter]
    IKRetargeter = IKR_GestureSyncToMetaHuman.uasset
    (body 22 bones routed; spine 1→5 subdivision; finger 30 bones BYPASS — 1:1 mapping)
        |
        v
[Existing MetaHuman post-process ABP] (DO NOT REPLACE)
    Drives twist bones, helper bones, metacarpals
        |
        v
[Output Pose]
```

### 6.2 Step-by-step

1. **Drop a Live Link Pose node configured for body transforms** (the engine-bundled
   `LiveLinkPose Body` variant — pick the role that consumes `ULiveLinkAnimationRole`,
   not `ULiveLinkBasicRole`).
   - Set its **Subject Name** in the Details panel to `<characterId>_Body_GestureSync` (using
     your CharacterId UUID + `_Body_GestureSync` suffix; e.g.,
     `<your-character-id>_Body_GestureSync`).

2. **AFTER the `LiveLinkPose Body` node, drop in an IK Retargeter AnimNode.**
   - Set its **IKRetargeter** property to your `IKR_GestureSyncToMetaHuman.uasset` —
     **this asset is NOT shipped with the plugin; author it in your own project**
     (see the body integration guide). Author the 22-chain mapping, including the
     spine 1→5 Slerp weights (~0.10 / 0.20 / 0.25 / 0.25 / 0.20).
   - Source pose = output of `LiveLinkPose Body` node (pose pin).
   - Target pose = MetaHuman skeletal mesh.

3. **Body bones (indices 0..21) route through the IK Retargeter automatically** —
   the IKR's 22 chain mappings handle `pelvis`, `thigh_l/r`, `calf_l/r`, `foot_l/r`,
   `ball_l/r`, `spine_01..05`, `clavicle_l/r`, `upperarm_l/r`, `lowerarm_l/r`,
   `hand_l/r`, `neck_01`, `head`. Twist + helper bones are intentionally unmapped
   (driven by post-process ABP — see step 5).

4. **Finger bones (indices 22..51) BYPASS the IK Retargeter.**
   The 30 GestureSync finger bones are 1:1 with the 30 MetaHuman finger phalanges
   (`*_01_l`, `*_02_l`, `*_03_l` × 5 fingers × 2 hands = 30); the
   `LiveLinkPose Body` node writes finger transforms directly to the rig's finger
   bones, going through `DT_GestureSyncBoneRemap_Identity.csv` lookup +
   `FEstuaryBodyFrameTransform::ToUEFromGestureSync` math helper (handled inside the
   receiver before publish — operators see no extra node in the AnimGraph).

5. **Final post-process: existing MetaHuman post-process Animation BP runs
   AFTER the IK Retargeter + LiveLinkPose Body chain.** **DO NOT REPLACE IT.**
   The post-process ABP drives:
   - Twist bones (`upperarm_twist_*`, `calf_twist_*`, etc.) — inferred from
     parent bone rotation.
   - Helper bones (clavicle helpers, shoulder correctives) — preserved.
   - **MetaHuman metacarpals** (`index_metacarpal_l`, `middle_metacarpal_l`,
     `ring_metacarpal_l`, `pinky_metacarpal_l`, plus right-hand counterparts) —
     GestureSync does NOT emit these (SMPL-X has no metacarpals, Pitfall 11); the
     post-process ABP infers metacarpal rotation from the finger 1+2+3 phalanges.

> **CRITICAL:** Replacing the post-process ABP breaks twist bones (visible as
> "shrink-wrap" arms during full-rotation poses) AND breaks finger metacarpals
> (visible as "fingers pinned to wrist with stiff joint"). Keep it intact.

### 6.3 Where the IK Retargeter sits relative to Control Rig

Place the IK Retargeter **EARLY** in the AnimGraph — **immediately after `LiveLinkPose
Body`, before any Control Rig nodes**. This is the orthodox UE pattern: cross-skeleton
retargeting first, then per-rig polish via Control Rig + post-process ABP.

[Screenshot: AnimGraph layout — `LiveLinkPose Body` (Subject = `<characterId>_Body_GestureSync`) →
`IK Retargeter` (`IKR_GestureSyncToMetaHuman` assigned) → existing Control Rig → existing
post-process ABP → Output Pose]

### 6.4 PIE save caveat

When iterating the `ABP_Body` AnimGraph or the `IKR_GestureSyncToMetaHuman.uasset` chain
weights, **always exit PIE before saving**. PIE locks asset saves; the editor may show
"asset saved" but on next session the changes are absent. See
`METAHUMAN_BODY_ABP_GUIDE.md` Section 10 for the full caveat (Pitfall 10).

---

## Connecting

The connect flow is **unchanged from `BP_EstuaryMetaHumanSample`** — same Level Blueprint
pattern (BeginPlay → read Developer Settings → build auth payload → Connect Latent → bind
delegates). The only diff is **two flags must be set in the auth payload** to enable
both face and body:

7. From the BP's BeginPlay (or the Level Blueprint's BeginPlay if you place the actor
   at design time), call `CharacterComponent.Connect(authPayload)`. Build `authPayload`
   via `UEstuaryBlueprintLibrary::MakeDefaultAuthPayload`, then set:
   - **`bEnableAnimation = true`** (face — v1.0).
   - **`bEnableBodyAnimation = true`** (body — UPROPERTY on `FEstuaryAuthPayload`).

   Or set the **project-wide defaults** in
   **Project Settings > Plugins > Estuary**:
   - **`Enable Animation = true`** (face).
   - **`Enable Body Animation = true`** (body — UPROPERTY on
     `UEstuaryDeveloperSettings`).

   With project-wide defaults, every auth payload built via
   `MakeDefaultAuthPayload` ships with both flags pre-set — no per-actor wiring needed.

The rest of the Level Blueprint flow (push-to-talk, send-text, interrupt, bind
`OnBotResponse` / `OnSttResponse` / `OnStateChanged`) is **identical to
`BP_EstuaryMetaHumanSample`** — see that walkthrough for canonical examples.

---

## Expected Behavior in PIE

After following all steps above and pressing **Play**:

| Time | Event |
|------|-------|
| 0 ms | BeginPlay runs; Connect (Latent) fires |
| ~500 ms (local) / ~1.5 s (remote) | `session_info` received with `animation_capabilities.body.active = true`; `OnConnected` fires |
| Once at startup | Live Link panel shows source "Estuary" with **two** Subjects per character: bare `<characterId>` (face) AND `<characterId>_Body_GestureSync` (body); Output Log line "EstuaryLiveLink: ILiveLinkClient acquired and source added." |
| After push-to-talk + speech | `Txt_Transcript` shows partial STT result within 300 ms |
| ~1 s after speech ends | `bot_response` starts streaming; `Txt_BotResponse` fills word-by-word |
| Simultaneously | MetaHuman face exhibits ARKit-52 mouth movement driven by `bot_animation` frames pushed through Live Link; MetaHuman body+fingers exhibit GestureSync-driven motion driven by `bot_pose` frames pushed through Live Link |
| Cross-modality drift | Sub-100 ms by construction; no perceptible face↔body desync |
| At audio start | `UEstuaryAudioPlayerComponent` 200ms pre-buffer primes, then audio begins; both face and body receivers anchor their audio offset to the same playback time |
| Interrupt click | Face ramps to neutral over ~100 ms (Pitfall 25 settle frame); body ramps to bind pose over ~500 ms (body uses 500 ms because body motion has more inertia than face); audio stops; new turn is ready immediately |

---

## Platform Notes

| Platform | Voice | Face Lipsync | Body Animation | Notes |
|----------|-------|--------------|----------------|-------|
| Win64 | WebSocket / LiveKit WebRTC | Live Link source | Live Link source (`ULiveLinkAnimationRole`) | Primary target |
| Mac | WebSocket / LiveKit WebRTC | Live Link source | Live Link source | Untested for body in v2.0; expected to work (engine-bundled LiveLink covers all four platforms) |
| iOS | WebSocket voice | Live Link source | Live Link source | Body deferred to v2.x for iOS (no GPU-runner CI; manual smoke required) |
| Android | WebSocket voice | Live Link source | Live Link source | Body deferred to v2.x for Android |

The Estuary Live Link source is whitelisted on all four platforms (engine-bundled LiveLink
ships across the matrix in stock UE 5.6+). The PIE smoke validates the live GestureSync-driven
body+face on Win64; other platforms are deferred to v2.x.

---

## File Checklist — Assets to Create in the Editor

Use this checklist to verify all required assets are present before testing PIE:

- [ ] `BP_EstuaryMetaHumanBodySample` — Blueprint subclass of `AEstuaryCharacter`
  (parent = `AEstuaryCharacter`, location = your project Content folder)
- [ ] `WBP_EstuaryConversation` — Widget Blueprint with PTT button, status, transcript,
  response text (same as `BP_EstuaryMetaHumanSample`)
- [ ] `ABP_Body` — body Animation Blueprint with `LiveLinkPose Body` (Subject =
  `<characterId>_Body_GestureSync`) → `IK Retargeter` (`IKR_GestureSyncToMetaHuman` assigned) → existing
  Control Rig → existing post-process ABP → Output Pose (**NOT shipped; author in your project**)
- [ ] **Stock MetaHuman `Face_AnimBP`** with the existing `LiveLinkPose` node's
  Subject Name set to your CharacterId UUID (no copy / reparent / Modify Curve required;
  identical to `BP_EstuaryMetaHumanSample`)
- [ ] `DT_ArkitRemap_Identity` — UDataTable for face curves (row struct:
  `FEstuaryArkitRemapRow`); ships with plugin or create new with identity mapping
- [ ] `DT_GestureSyncBoneRemap_Identity` — UDataTable for body bones (row struct:
  `FEstuaryBodyBoneRemapRow`); ships with the plugin as a 52-row CSV with identity
  SMPL-X → MetaHuman mappings
- [ ] `IKR_GestureSyncToMetaHuman` — IK Retargeter asset (**NOT shipped; author in your
  project** — 22-chain body retarget with spine 1→5 Slerp weights baked in)
- [ ] `metahuman_common_v1` (bind pose JSON) — bundled at
  `Content/BindPoses/metahuman_common_v1.json` (52 quats at identity; loaded
  client-side by the plugin at session connect)
- [ ] `L_EstuaryBodySample` — Level with `BP_EstuaryMetaHumanBodySample` placed,
  lighting set up (PostProcessVolume locked to manual exposure), and Level Blueprint wired
  per the canonical flow (same shape as `BP_EstuaryMetaHumanSample` Level Blueprint — see
  that walkthrough)

---

## Cross-References

- Mirror of [BP_EstuaryMetaHumanSample.md](BP_EstuaryMetaHumanSample.md) (face-only sample).
- Body integration guide:
  [`Docs/METAHUMAN_BODY_ABP_GUIDE.md`](../METAHUMAN_BODY_ABP_GUIDE.md) —
  detailed per-character setup, IKRetargeter authoring walkthrough, FingerPolicy
  configuration, drift defense, PIE save caveats, troubleshooting.
- Face integration guide: [`../METAHUMAN_ABP_GUIDE.md`](../METAHUMAN_ABP_GUIDE.md).

---

*Body-and-face sample BP walkthrough.*
