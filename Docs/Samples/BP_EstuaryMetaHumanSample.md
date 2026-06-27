# BP_EstuaryMetaHumanSample — Blueprint Design Spec

## Overview

`BP_EstuaryMetaHumanSample` is a Blueprintable `AEstuaryCharacter` subclass that demonstrates
the complete Estuary voice + ARKit-52 lipsync + push-to-talk conversation flow in a single
placeable actor. It is the reference implementation of the canonical Level Blueprint pattern
described in the SDK contract.

**Why a design spec instead of a `.umap`:** Editor-generated assets (`.umap`, `.uasset`,
`.uasset` Blueprint binaries) cannot be shipped inside a code-only plugin because they are
binary UE-serialised files tied to specific engine versions and project content directories.
This spec gives a designer the complete specification to recreate the sample map in the Editor
in approximately 10 minutes (down from 15 — Live Link makes face setup faster). Follow it
step-by-step and the result will behave identically to a shipped sample map.

---

## Actor Class

| Property | Value |
|----------|-------|
| **Parent class** | `AEstuaryCharacter` (Estuary plugin) |
| **Blueprint name** | `BP_EstuaryMetaHumanSample` |
| **Content Browser location** | your project Content folder |
| **Tick enabled** | Yes (inherited from `AEstuaryCharacter`) |

---

## Default Sub-Objects (auto-created by AEstuaryCharacter)

These components are created in the `AEstuaryCharacter` C++ constructor. **Do NOT add
them manually** — doing so creates duplicates. They appear in the Blueprint's component
hierarchy automatically once you reparent to `AEstuaryCharacter`.

| Component | Class | Purpose |
|-----------|-------|---------|
| `CharacterComponent` | `UEstuaryCharacterComponent` | Sends `text` events; receives streaming `bot_response` and `interrupt` callbacks. Hosts `LiveLinkSubjectOverride` (optional FName disambiguator). |
| `MicrophoneComponent` | `UEstuaryMicrophoneComponent` | Cross-platform mic capture via Runtime Audio Importer; emits `start_voice`, `stream_audio`, `stop_voice` |
| `AudioPlayerComponent` | `UEstuaryAudioPlayerComponent` | Decodes base64 PCM16 `bot_voice` chunks into `USoundWaveProcedural` ring buffer; 200ms pre-buffer; interrupt flush |
| `AnimationReceiver` | `UEstuaryAnimationReceiver` | Buffers 30fps ARKit-52 `bot_animation` frames; audio-clock-driven linear interpolation; publishes to `EstuaryLiveLink` source |

---

## MetaHuman Setup (add in Blueprint Details)

### Body Mesh

1. In the Blueprint editor, add a **Skeletal Mesh Component** to the actor root.
   - **Skeletal Mesh**: assign your MetaHuman Body mesh (from your MetaHuman content folder)
   - **Anim Class**: your MetaHuman's `ABP_Body` (or leave default)
   - **Name it**: `BodyMesh`
   - Position it at the actor root (no offset needed for a desktop demo)

### Face Mesh

2. Add a second **Skeletal Mesh Component** as a **child** of `BodyMesh`.
   - **Skeletal Mesh**: your MetaHuman Face skeletal mesh (`m_med_nrw_head_lod0_mesh` or
     equivalent — use LOD0 for the sample to get full blend shape resolution)
   - **Anim Class**: the **stock MetaHuman `Face_AnimBP`** — **no reparent required**.
     Path B (Live Link) reads curves directly through the existing `LiveLinkPose` node
     in the stock AnimBP. See `Docs/METAHUMAN_ABP_GUIDE.md` for the
     one-step Subject Name field you need to set on that node.
   - **Name it**: `FaceMesh`
   - Leave Leader Pose Component set to `BodyMesh` so face follows body transforms

### Animation Receiver Wiring

3. In the `AnimationReceiver` component Details panel (Blueprint Defaults):
   - Set **Remap Asset** to `DT_ArkitRemap_Identity` (imported from the plugin's
     `Content/DataTables/` — or create a new `UDataTable` with `FEstuaryArkitRemapRow`
     row struct if you need a non-identity profile). Identity is correct for stock
     MetaHumans (post-5.2 ARKit-Ready variant).

4. **No Event-Graph wiring is required for face animation under Path B.** The receiver
   resolves `FEstuaryLiveLinkBridge` through `UEstuarySubsystem` automatically each
   tick and publishes ramped/interpolated frames to the publisher. (The legacy v0.1
   `TargetAnimInstance.Set` step from the old guide is no longer applicable.)

### Character ID

5. In the Blueprint **Defaults** panel, find **Character Id** (on `AEstuaryCharacter`):
   - Set it to the UUID of your Estuary character
   - Example: `"xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"`
   - This is the `character_id` used in the `authenticate` handshake **and** the
     default Live Link Subject Name (resolution order: Override → CharacterId →
     OwnerActor name).

### Face AnimBP — single Live Link step

6. Open your MetaHuman's `Face_AnimBP` (stock asset; no reparent). Click the existing
   `LiveLinkPose` AnimGraph node, set **Live Link Subject Name** in the Details panel
   to your CharacterId UUID. Compile + Save. **Single screenshot setup**: that one
   field is the entire face wiring step.

---

## UMG Widget — WBP_EstuaryConversation

Add a **Widget Component** to `BP_EstuaryMetaHumanSample` for the minimal conversation UI.

| Component | Class | Space |
|-----------|-------|-------|
| `ConversationWidget` | `UWidgetComponent` | `Screen` (simpler for a demo; switch to `World` for diegetic UI) |

### Create WBP_EstuaryConversation (new Widget Blueprint)

In the Content Browser: **right-click → User Interface → Widget Blueprint → name it
`WBP_EstuaryConversation`**. Add these elements to the canvas:

| Widget | Name | Type | Purpose |
|--------|------|------|---------|
| Button | `Btn_Connect` | `UButton` | Triggers Connect on click (optional if connecting in BeginPlay) |
| Button | `Btn_PushToTalk` | `UButton` | `OnPressed` → `StartListening`; `OnReleased` → `StopListening` |
| TextBlock | `Txt_Status` | `UTextBlock` | Displays current session state ("Connecting…", "Connected", "Error") |
| TextBlock | `Txt_BotResponse` | `UTextBlock` | Streaming bot response text (append per chunk; clear on new turn) |
| TextBlock | `Txt_Transcript` | `UTextBlock` | Partial / final STT transcript text |
| TextBox | `Input_UserText` | `UEditableTextBox` | Optional text input for text-chat mode |
| Button | `Btn_SendText` | `UButton` | Sends `Input_UserText.Text` via `CharacterComponent.SendText` |

Set `Txt_BotResponse` to **Auto-Wrap** and give it enough vertical space for 3–4 lines.

---

## Level Blueprint — Canonical Flow (SAMPLE-02)

The Level Blueprint implements the full conversation flow. Each numbered section below
corresponds to a logical block in the Event Graph.

### Event BeginPlay

**Block 1 — Get actor reference**

1. Use **Get All Actors Of Class** (Class = `BP_EstuaryMetaHumanSample`) → get element 0
   - Store as variable `EstuaryActor`
   - (Or assign a direct reference if the actor is placed at design time)

**Block 2 — Read server settings from Project Settings**

2. Call **Get Default Object** (Class = `UEstuaryDeveloperSettings`)
   - Read `Server URL` → store as `ServerUrl`
   - Read `Audio Sample Rate` → store as `SampleRate`

**Block 3 — Build auth payload**

3. Call **Make Default Auth Payload** (from `UEstuaryBlueprintLibrary`):
   - `ApiKey`: your Estuary API key — store as a Level Blueprint variable for development;
     **never commit this value to source control**
   - `CharacterId`: same UUID as the actor's **Character Id** property
   - `PlayerId`: `"player-001"` (or use a dynamic player ID from your game state)
   - `bEnableAnimation`: `true`
   - `SampleRate`: from Developer Settings (24000 default; A2F now accepts the negotiated
     rate without coercion).

**Block 4 — Connect (Latent)**

4. Call **Connect (Latent)** on `EstuaryActor.CharacterComponent` (or on the Subsystem):
   - `Server URL`: from step 2
   - `Auth`: from step 3
   - **OnConnected** execution pin → proceed to Block 5
   - **OnAuthFailed** pin → Print String "Auth failed: " + error; optionally show UI
   - **OnRejected** pin → Print String "Session rejected: " + reason; optionally show UI
   - **OnNetworkError** pin → Print String "Network error"; schedule retry if desired

### After OnConnected Fires

**Block 5 — Bind bot response**

5. Bind `EstuaryActor.CharacterComponent.OnBotResponse` to a Custom Event:
   - Custom Event name: `OnBotResponseReceived`
   - Input: `FEstuaryBotResponse Response`
   - Body: **Append** `Response.Text` to `Txt_BotResponse`
   - If `Response.bIsFinal = true`: schedule a 5-second **Delay** then clear `Txt_BotResponse`

**Block 6 — Bind STT transcript**

6. Bind `EstuaryActor.CharacterComponent.OnSttResponse` to a Custom Event:
   - Custom Event name: `OnSttReceived`
   - Input: `FEstuarySttResponse Response`
   - Body: Set `Txt_Transcript.Text` = `Response.Text`
   - If `Response.bIsFinal = true`: optionally fade out transcript after 3 seconds

**Block 7 — Bind session state**

7. Get **UEstuarySubsystem** (via `Get Estuary Subsystem` from `UEstuaryBlueprintLibrary`
   or from the Level Blueprint's `Get Game Instance → Get Subsystem`):
   - Bind `Subsystem.OnStateChanged` to a Custom Event
   - Custom Event name: `OnEstuaryStateChanged`
   - Input: `EEstuarySessionState NewState`
   - Body: **Switch on EEstuarySessionState** → set `Txt_Status.Text` for each value:
     - `Disconnected` → "Disconnected"
     - `Connecting` → "Connecting…"
     - `Connected` → "Connected"
     - `Reconnecting` → "Reconnecting…"
     - `Error` → "Connection Error"

### Push-to-Talk (Btn_PushToTalk Events)

8. In `WBP_EstuaryConversation` event graph:
   - `Btn_PushToTalk.OnPressed` → call `EstuaryActor.MicrophoneComponent.StartListening()`
   - `Btn_PushToTalk.OnReleased` → call `EstuaryActor.MicrophoneComponent.StopListening()`

### Interrupt (Barge-In)

9. Add an **Interrupt** button to the widget, or bind to a keyboard key (e.g., **I** key):
   - `OnClicked` / key pressed → call `EstuaryActor.CharacterComponent.Interrupt()`
   - This stops TTS playback, ramps the LiveLink-driven face to neutral over ~100 ms,
     signals the server to abort the in-flight response, and allows the user to start
     a new turn

### Send Text (Text-Chat Mode)

10. In `WBP_EstuaryConversation`:
    - `Btn_SendText.OnClicked` → call `EstuaryActor.CharacterComponent.SendText(Input_UserText.Text)`
    - Clear `Input_UserText` after sending
    - This exercises the text-chat path alongside (or instead of) voice input

---

## Expected Behavior in PIE

After following all steps above and pressing **Play**:

| Time | Event |
|------|-------|
| 0 ms | BeginPlay runs; Connect (Latent) fires |
| ~500 ms (local) / ~1.5 s (remote) | `session_info` received; `OnConnected` fires; `Txt_Status` = "Connected" |
| Once at startup | Live Link panel shows source "Estuary"; Output Log line "EstuaryLiveLink: ILiveLinkClient acquired and source added." |
| After push-to-talk + speech | `Txt_Transcript` shows partial STT result within 300 ms |
| ~1 s after speech ends | `bot_response` starts streaming; `Txt_BotResponse` fills word-by-word |
| Simultaneously | MetaHuman face exhibits ARKit-52 mouth movement driven by `bot_animation` frames pushed through Live Link |
| At audio start | `UEstuaryAudioPlayerComponent` 200ms pre-buffer primes, then audio begins |
| Interrupt click | Face ramps to neutral over ~100 ms (Pitfall 25 settle frame); audio stops; new turn is ready immediately |

---

## Platform Notes

| Platform | Voice | Lipsync (Live Link) | Notes |
|----------|-------|---------------------|-------|
| Win64 | WebSocket (fallback) or LiveKit WebRTC | Live Link source + LiveLinkPose node | LiveKit requires Step 5 (prebuilts in ThirdParty/) |
| Mac | WebSocket (fallback) or LiveKit WebRTC | Live Link source + LiveLinkPose node | Fat binary: arm64 + x86_64 |
| iOS | WebSocket voice | Live Link source + LiveLinkPose node | AVAudioSession shim auto-included; LiveKit deferred to v1.1 |
| Android | WebSocket voice | Live Link source + LiveLinkPose node | Call `AcquireMicrophonePermission` before `StartListening()` |

The Estuary Live Link source is whitelisted on all four platforms (engine-bundled
LiveLink ships across the matrix in stock UE 5.6).

---

## File Checklist — Assets to Create in the Editor

Use this checklist to verify all required assets are present before testing PIE:

- [ ] `BP_EstuaryMetaHumanSample` — Blueprint subclass of `AEstuaryCharacter`
  (parent = `AEstuaryCharacter`, location = your project Content folder)
- [ ] `WBP_EstuaryConversation` — Widget Blueprint with PTT button, status, transcript, response text
- [ ] **Stock MetaHuman `Face_AnimBP`** with the existing `LiveLinkPose` node's
  Subject Name set to your CharacterId UUID (no copy / reparent / Modify Curve required)
- [ ] `DT_ArkitRemap_Identity` — UDataTable (row struct: `FEstuaryArkitRemapRow`); import
  from plugin or create new with identity mapping (each ARKit-52 curve maps to itself)
- [ ] `L_EstuarySample` — Level with `BP_EstuaryMetaHumanSample` placed, lighting set up,
  and Level Blueprint wired per the canonical flow above
