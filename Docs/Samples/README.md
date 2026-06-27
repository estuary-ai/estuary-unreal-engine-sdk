# Estuary MetaHuman Sample — Setup Guide

This guide walks you through adding the Estuary plugin to a UE 5.6+ project and running a
real-time voice conversation with a MetaHuman character that has ARKit-52 lipsync. By the
end you will have a playable in-editor demo where the MetaHuman mouth moves in sync with
streamed TTS audio from the Estuary gateway.

## Prerequisites

- Unreal Engine 5.6 or later
- A running Estuary backend, or access to `https://api.estuary-ai.com` with an API key
- Git (for cloning plugin dependencies as submodules)
- Visual Studio 2022 (Win64) or Xcode 15+ (Mac) for C++ project compilation

## Step-by-Step Setup

### Step 1: Create or open a UE 5.6+ project

- Third Person template recommended — it provides a pawn and character skeleton you can
  reference when positioning the MetaHuman
- Blank also works; all Estuary classes are self-contained
- Minimum UE version: 5.6.0 (the engine-bundled `LiveLink` plugin powers Path B lipsync;
  earlier engines lack the LiveLinkPose AnimGraph node we depend on out of the box)
- C++ project required (the plugin compiles C++ source; Blueprint-only projects must be
  converted first)

### Step 2: Add the Estuary plugin

```bash
cd <YourProject>/Plugins
git submodule add https://github.com/estuary-ai/estuary-unreal-sdk Estuary
```

Then right-click your `.uproject` in Explorer → **Generate Visual Studio project files**
(or run `UnrealBuildTool` from command line), and build the project.

The plugin auto-enables on first build. Verify in **Edit → Plugins → search "Estuary" →
Enabled**. The engine-bundled **Live Link** plugin auto-enables transitively because
the Estuary plugin lists it as a dependency in `Estuary.uplugin`.

### Step 3: Install SocketIOClient-Unreal v2.11.0

```bash
cd <YourProject>/Plugins
git submodule add https://github.com/getnamo/SocketIOClient-Unreal SocketIOClient
git -C SocketIOClient checkout v2.11.0
```

After adding the submodule, re-run **Generate Visual Studio project files** and rebuild.
Enable in **Edit → Plugins → SocketIOClient → Enabled → restart editor**.

### Step 4: Install Runtime Audio Importer from Fab

Runtime Audio Importer (RAI) provides cross-platform microphone capture.
UE's built-in `UAudioCaptureComponent` is Windows-only; RAI covers Win64, Mac, iOS, and Android.

1. Open **Epic Games Launcher → Library → Fab**
2. Search **"Runtime Audio Importer"** by Georgy Treshchev
3. Click **Add to Engine** (or Add to Project if you prefer a project-local install)
4. In-editor: **Edit → Plugins → search "Runtime Audio Importer" → Enable → restart editor**

Fab listing: https://www.fab.com/listings/66e0d72e-982f-4d9e-aaaf-13a1d22efad1

### Step 5: (Optional) Enable LiveKit WebRTC voice on Win64 / Mac desktop

Drop `livekit/client-sdk-cpp` prebuilts into the plugin's `ThirdParty/` directory:

```
Plugins/Estuary/ThirdParty/
  livekit-client-sdk-cpp/
    include/
    lib/
      Win64/   ← .lib files
      Mac/     ← .dylib / .a files
```

`EstuaryLiveKit.Build.cs` auto-detects this directory and defines `WITH_ESTUARY_LIVEKIT=1`.
When the directory is absent, the module still compiles and loads — voice falls back to
WebSocket audio transparently.

**Skip this step for mobile builds** (iOS/Android use WebSocket voice in v1).

If you skip this step, the editor will show a non-blocking notification toast at startup
("Estuary: LiveKit WebRTC voice is unavailable…") — this is expected and informational.

### Step 6: Configure Project Settings

1. In-editor: **Edit → Project Settings → Plugins → Estuary**
2. Set **Server URL**: `https://api.estuary-ai.com` (the default; or your local gateway URL)
3. Set **Audio Sample Rate**: `24000` (default; 16000 also supported. NVIDIA Audio2Face
   accepts both rates.)
4. Set **Enable Animation**: `true` (enables the `bot_animation` ARKit-52 stream)
5. Leave **JPEG Quality** at default (`85`) unless you have specific VLM performance needs

**Security note:** Do NOT commit your API key to version control. Pass it at runtime in
the Level Blueprint via the **Connect (Latent)** node's Auth input. Store keys in
environment variables or UE's encrypted config system for non-development builds.

### Step 7: Build the BP_EstuaryMetaHumanSample Blueprint actor

Follow the design spec in `BP_EstuaryMetaHumanSample.md` (same folder as this file) to
create the sample Blueprint in the Editor. This takes approximately 10 minutes.

Alternatively, create a Blueprint subclass of `AEstuaryCharacter` directly:

- Set **CharacterId** to your Estuary character's UUID in the Blueprint Defaults panel
- The plugin auto-creates `UEstuaryCharacterComponent`, `UEstuaryMicrophoneComponent`,
  `UEstuaryAudioPlayerComponent`, and `UEstuaryAnimationReceiver` as default sub-objects
- No manual component attachment is needed unless you are customising component settings

### Step 8: Wire the MetaHuman face AnimBP (one step)

Path B (Live Link) collapses the prior multi-step Modify Curve setup to a single field.
Open your MetaHuman's stock `Face_AnimBP`, click the existing `LiveLinkPose` AnimGraph
node, and set its **Live Link Subject Name** to your character's UUID (the same value
as the `CharacterId` you set on the actor). Compile + Save. **No reparent, no Modify
Curve, no 52 Property Access bindings.**

Full guide with subject-name override and custom-rig remap notes:
`Docs/METAHUMAN_ABP_GUIDE.md`.

### Step 9: Run PIE (Play In Editor)

1. Place your `AEstuaryCharacter` Blueprint in the level
2. In the **Level Blueprint**:
   - **BeginPlay** → **Connect (Latent)** node (wire in `ServerUrl` and the `Auth` payload
     built with `Make Default Auth Payload` from `UEstuaryBlueprintLibrary`)
   - **OnConnected** pin → bind `CharacterComponent.OnBotResponse` to a display event
3. Press **Play (PIE)**
4. Click the **Push-to-Talk** button (or call `MicrophoneComponent.StartListening()`)
5. Speak — you should see the MetaHuman mouth move and hear the TTS response within 1–2 s

Expected latency on a local gateway: ~300 ms to first STT response, ~1 s to first TTS audio.
Expected latency on `api.estuary-ai.com`: ~1 s to first STT, ~2 s to first TTS audio.

To verify Live Link is wired correctly, open **Window → Virtual Production → Live Link**.
You should see one source named **Estuary** with status **Active**, and one Subject per
active character (Subject names match the resolution order:
`LiveLinkSubjectOverride` → `CharacterId` → `OwnerActor->GetFName()`).

## Troubleshooting

| Symptom | Likely Cause | Fix |
|---------|--------------|-----|
| No sound | Missing RAI plugin | Confirm RAI is installed and enabled |
| Face doesn't move | LiveLinkPose Subject Name wrong or empty | Open `Face_AnimBP`, set node's Subject Name to your CharacterId UUID, compile + save |
| Face freezes mid-expression on interrupt | Older Estuary plugin without ramp loop | Update to a plugin version with the interrupt ramp + zero settle frame (Pitfall 25) |
| Live Link panel shows no "Estuary" source | LiveLink engine plugin not loaded yet OR EstuaryLiveLink module not registered | Check Output Log for "EstuaryLiveLink: ILiveLinkClient acquired and source added." — if missing, look for a deferred-registration line; restart editor if needed |
| Connection fails immediately | Wrong ServerUrl or invalid ApiKey | Check Project Settings ServerUrl (must start `wss://`); verify API key is valid |
| Connection times out | Firewall blocking WebSocket | Ensure port 443 is open for `wss://` connections |
| iOS mic is silent | Missing AVAudioSession shim | The plugin's `EstuaryAudioSession.mm` Obj-C shim is auto-included; check that iOS platform build rules include Estuary's Resources/IOS/ |
| Android mic is silent | Missing runtime permission | Call `AcquireMicrophonePermission` (latent node) before `StartListening()` |
| LiveKit notification in editor | livekit-client-sdk-cpp not in ThirdParty/ | Expected when Step 5 was skipped; WebSocket voice is active and functional |
| "SocketIOClient not found" build error | SocketIOClient plugin missing or wrong version | Confirm `Plugins/SocketIOClient/` exists and is on tag v2.11.0 |

## Next Steps

- Read `../../METAHUMAN_ABP_GUIDE.md` for the full Live Link integration guide
  (custom rigs, subject-name override, migration from v0.1 Modify Curve)
- Read `../../README.md` for the full SDK parity status, platform matrix, and API reference
- See `BP_EstuaryMetaHumanSample.md` (this folder) for the Blueprint design spec
