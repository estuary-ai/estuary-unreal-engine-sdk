# Estuary Unreal Engine SDK

**Real-time AI voice + text conversation for Unreal Engine 5.6+ — MetaHuman ARKit-52 lipsync,
streaming TTS, microphone STT, scene-vision VLM, and optional LiveKit WebRTC, with a
Blueprint-callable public API for designers.**

## v0.2 — Live Link integration

ARKit-52 lipsync delivery in the UE SDK has migrated from the v0.1
`UEstuaryFaceAnimInstance` UPROPERTY + Modify Curve pipeline to a Live Link source
(the new `EstuaryLiveLink` runtime module). Per-character setup is now a single
**Subject Name** field on the existing `LiveLinkPose` AnimGraph node in
MetaHuman `Face_AnimBP`. No reparenting, no Modify Curve node, no 52 Property
Access bindings. UE 5.6+ recommended (engine-bundled `LiveLink` plugin auto-enables
via `Estuary.uplugin`).

**Migration:** if you reparented `Face_AnimBP` to `UEstuaryFaceAnimInstance` under
v0.1, see `Docs/METAHUMAN_ABP_GUIDE.md` →
*Migration from v0.1 (Modify Curve)*. The class remains as a deprecated empty
stub for one minor version (so reparented user blueprints continue to load); it
is scheduled for removal in v2.

**No backend / cross-SDK changes:** the `bot_animation` Socket.IO event payload
is identical pre- and post-migration (`SDK_CONTRACT.md` is byte-for-byte
unchanged). Unity, Lens Studio, TypeScript, and web SDKs are unaffected — Live
Link is a UE-only consumer adapter.

## Features

- Socket.IO v4 transport to Estuary gateway over WebSocket (required)
- Streaming TTS playback via `USoundWaveProcedural` ring buffer — pop-free, interrupt-safe
- Cross-platform microphone capture via Runtime Audio Importer (Win64 / Mac / iOS / Android)
- ARKit-52 MetaHuman lipsync at 30 fps via Live Link source — audio-clock-driven, drift < 100 ms
- Scene-capture VLM: render-target readback → JPEG → `camera_image` emit
- Optional LiveKit WebRTC voice on Win64 and Mac (auto-detected; WebSocket fallback when absent)
- Blueprint-first API: every op is a `BlueprintCallable` UFUNCTION or latent async node
- Multi-character scenes: two or more `AEstuaryCharacter` actors with independent sessions

## Platform Matrix

| Feature | Win64 | Mac | iOS | Android |
|---------|-------|-----|-----|---------|
| Core runtime | v1 | v1 | v1 | v1 |
| Socket.IO / WebSocket voice | v1 | v1 | v1 | v1 |
| Microphone (Runtime Audio Importer) | v1 | v1 | v1 | v1 |
| TTS playback | v1 | v1 | v1 | v1 |
| ARKit-52 lipsync (MetaHuman) | v1 | v1 | v1 | v1 |
| Scene capture VLM | v1 | v1 | v1 | v1 |
| Optional LiveKit WebRTC | v1 | v1 | v1.1 | v1.1 |
| Editor tools | v1 | v1 | — | — |

LiveKit on iOS/Android requires cross-compiling from `livekit/rust-sdks` — deferred to v1.1.
WebSocket voice fallback covers mobile in v1.

## Install

A bare clone compiles in **stub mode** — `Connect`, voice, and mic are no-ops until the two
required dependencies are present (there is no auto-installer). Install them, then rebuild.

```bash
# From your UE 5.6+ project root
cd Plugins
git clone https://github.com/estuary-ai/estuary-unreal-sdk Estuary

# Required dependency 1 — getnamo SocketIOClient (Socket.IO v4 transport)
git clone --depth 1 --branch v2.11.0 https://github.com/getnamo/SocketIOClient-Unreal SocketIOClient
```

**Required dependency 2 — Runtime Audio Importer** (cross-platform microphone capture). Install
from Fab and choose **Add to Project** — *not* "Add to Engine": the build only detects a
project-local `Plugins/RuntimeAudioImporter/`.
<https://www.fab.com/listings/66e0d72e-982f-4d9e-aaaf-13a1d22efad1>

Then right-click your `.uproject` → **Generate Visual Studio project files**, rebuild, and open
the editor. With both dependencies present, `WITH_ESTUARY_SOCKETIO=1` and `WITH_ESTUARY_RAI=1`
are defined automatically and full functionality is live. If either is missing, the plugin still
compiles and the editor shows a single non-blocking toast; the affected feature stays a no-op
until you install it.

**Prerequisites:**
- Unreal Engine 5.6+ (LiveLinkPose AnimGraph node + AudioExtensions)
- Visual Studio 2022 with the "Game development with C++" workload (Windows)

## Optional LiveKit WebRTC (Win64 + Mac)

Run the prebuilt fetcher once per workstation. It reproduces the upstream
`LiveKitSDK.cmake` helper's logic without requiring a CMake toolchain, downloads
the pinned `v0.3.4` release asset for the current host triple, and lays it out at
`Plugins/Estuary/ThirdParty/livekit-client-sdk-cpp/` with a `.version` sentinel for
cache invalidation:

- Windows: `powershell -ExecutionPolicy Bypass -File Plugins/Estuary/ThirdParty/Setup.ps1`
- macOS:   `bash Plugins/Estuary/ThirdParty/Setup.sh`

Resulting layout (flat — no `Win64/` or `Mac/` subdirectories):

```
Plugins/Estuary/ThirdParty/livekit-client-sdk-cpp/
  include/livekit/   ← public headers
  lib/               ← static-link surface + cmake helpers
  bin/               ← runtime DLLs (Windows only)
  .version           ← v0.3.4 sentinel
```

`EstuaryLiveKit.Build.cs` auto-detects the directory and defines `WITH_ESTUARY_LIVEKIT=1`.
When absent, the module still compiles and loads; voice falls back to WebSocket audio
transparently.

## Quickstart (5 lines in Level Blueprint)

1. Add an `AEstuaryCharacter` Blueprint subclass to your level; set `CharacterId`
2. Add **Connect (Latent)** node in Level Blueprint Event BeginPlay
3. Wire `OnConnected` → bind `OnBotResponse` on `CharacterComponent`
4. Add a Push-to-Talk button: `OnPressed` → `MicrophoneComponent.StartListening()`; `OnReleased` → `StopListening()`
5. Press PIE — speak — hear the bot with visible MetaHuman lipsync

Expected result: voice conversation with ARKit-52 lipsync within one PIE run.

## Sample and Guides

- `Docs/Samples/README.md` — Step-by-step setup walkthrough (9 steps)
- `Docs/Samples/BP_EstuaryMetaHumanSample.md` — Blueprint design spec for the sample actor
- `Docs/METAHUMAN_ABP_GUIDE.md` — MetaHuman face AnimBP wiring guide

## Parity Status (vs `../SDK_CONTRACT.md`)

Contract Version: 1.0 | Transport: Socket.IO v4 / `/sdk` namespace

| Event / Capability | Status | Notes |
|--------------------|--------|-------|
| `authenticate` (connect handshake) | Shipped (Phase 2) | `UEstuarySubsystem::Connect`; auth payload → `FEstuaryAuthPayload` |
| `session_info` | Shipped (Phase 2) | `FOnEstuarySessionConnected` delegate |
| `auth_error` | Shipped (Phase 2) | `FOnEstuaryAuthError` delegate |
| `session_rejected` | Shipped (Phase 2) | `FOnEstuarySessionRejected` delegate; reason/cap surfaced |
| `error` | Shipped (Phase 2) | `FOnEstuaryError` delegate (`FEstuaryError {Message}`); non-terminal — session stays open |
| `quota_exceeded` | Shipped | `FOnEstuaryQuotaExceeded` delegate (`FEstuaryQuotaExceeded {ErrorCode, Message, Current, Limit, Remaining, Tier}`); tolerates the share-limit variant (numeric fields omitted); server disconnects right after |
| `text` (user_text) | Shipped (Phase 3) | `UEstuaryCharacterComponent::SendText`; generates `message_id` |
| `bot_response` | Shipped (Phase 3) | `FOnEstuaryBotResponse`; `bIsFinal`, `chunk_index`, `is_interjection` surfaced |
| `update_preferences` | Shipped (Phase 3) | `UEstuaryCharacterComponent::UpdatePreferences` |
| `client_interrupt` | Shipped (Phase 3) | `UEstuaryCharacterComponent::Interrupt` |
| `interrupt` (server→client ack) | Shipped (Phase 3) | `FOnEstuaryInterruptAcked` delegate |
| `bot_voice` | Shipped (Phase 4) | `UEstuaryAudioPlayerComponent` ring buffer; base64 PCM16 |
| `audio_playback_complete` | Shipped (Phase 4) | Emitted on ring-buffer drain; tail-detection in `TickComponent` |
| `start_voice` / `stop_voice` | Shipped (Phase 5) | `UEstuaryMicrophoneComponent::StartListening` / `StopListening` |
| `stream_audio` | Shipped (Phase 5) | SPSC queue → game-thread tick → base64 emit |
| `stt_response` | Shipped (Phase 5) | `FOnEstuarySttResponse`; `bIsFinal`; auto-barge-in on partial |
| `bot_animation` (ARKit-52) | Shipped (Phase 6) | `UEstuaryAnimationReceiver` + `UEstuaryFaceAnimInstance`; 52 float UPROPERTYs |
| `camera_image` | Shipped (Phase 7) | `UEstuaryCameraComponent::SendCameraFrame`; consent-gated |
| `camera_capture` | Shipped (Phase 7) | Server-initiated; auto-triggers `SendCameraFrame` if consent=true |
| `livekit_token` (request) | Shipped (Phase 8) | `UEstuarySubsystem::RequestLiveKitToken` |
| `livekit_token` (server→client) | Shipped (Phase 8) | Triggers `IEstuaryLiveKitVoice::JoinRoom` when module loaded |
| `livekit_join` | Shipped (Phase 8) | Emitted after successful `JoinRoom` |
| `livekit_leave` | Shipped (Phase 8) | `UEstuarySubsystem::LeaveLiveKitRoom` |
| `livekit_ready` | Shipped (Phase 8) | Handled; transitions voice mode to LiveKit |
| `livekit_error` | Shipped (Phase 8) | Logs error; keeps voice mode as WebSocket |
| `say_line` | Shipped | `UEstuaryCharacterComponent::SayLine(Text, bTextOnly)` → `SendSayLine` → `EmitSayLine`; wire `{text, text_only}` (snake_case `text_only`); server auto-interrupts + mints its own `message_id`; returns void (response via bot_response/bot_voice) |
| `voice_push_to_talk` | Shipped (Phase 5) | `UEstuaryMicrophoneComponent::StartListening` / `StopListening` are the client-gated PTT window over `start_voice` / `stream_audio` / `stop_voice` |
| `video_frame` (WebSocket) | Deferred (v2) | v1 ships single-frame `camera_image` only |
| `memory_updated` | Shipped | `FOnEstuaryMemoryUpdated` delegate (`FEstuaryMemoryUpdated` + `FEstuaryMemoryData[]`); top-level snake_case, `new_memories[]` items camelCase (only camelCase-array payload in the SDK) |
| `client_action` (contract v1.10) | Shipped | `UEstuaryCharacterComponent::OnClientAction` delegate (`FEstuaryClientAction`); typed in-world action calls via native LLM function calling (SCRUM-202). Exactly once per call, fire-on-arrival (not synced to TTS); `Arguments` stringified `TMap<FString,FString>` (numbers invariant, booleans lowercase). The plugin declares `capabilities.client_action: true` on connect — required, since the server serves the legacy XML tag path to clients that don't |
| encounter / character / share REST APIs | Out of scope | Host-app/dashboard concern (Firebase / dev-API-key auth, character authoring + share minting); UE SDK is a realtime conversation runtime with no HTTP module by design |
| encounter Socket.IO (`subscribe_encounter` / `encounter_*`) | Deferred | Lens Studio only at MVP; two-NPC-room feature, low fit for a single-MetaHuman SDK |

## Contributing / License

See `LICENSE` for terms. Third-party component notices: `THIRD_PARTY_NOTICES.md`.

Parity questions or SDK contract changes: diff `../SDK_CONTRACT.md` and update this table.
