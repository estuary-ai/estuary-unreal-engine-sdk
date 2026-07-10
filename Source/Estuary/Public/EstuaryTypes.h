// EstuaryTypes.h — Wire payload USTRUCTs and session state UENUM for the Estuary Unreal SDK.
// Pitfall 23: #pragma once, no CoreMinimal.h, no Engine.h, IWYU-only includes.

#pragma once

#include "CoreTypes.h"
#include "UObject/ObjectMacros.h"
#include "Containers/UnrealString.h"
#include "Containers/Array.h"
#include "Containers/Map.h"

#include "EstuaryTypes.generated.h"

// ---------------------------------------------------------------------------
// Session state machine — used by UEstuarySubsystem and surfaced to Blueprint.
// Every transition broadcasts FOnEstuarySessionStateChanged (Plan 03).
// ---------------------------------------------------------------------------

/** Current lifecycle state of an Estuary session.
 *
 *  Transitions (Plan 03 enforces these):
 *    Disconnected  -> Connecting    (Connect() called)
 *    Connecting    -> Connected     (session_info received)
 *    Connecting    -> Error         (auth_error / session_rejected received)
 *    Connected     -> Reconnecting  (transport closed, non-1000 code)
 *    Reconnecting  -> Connecting    (backoff timer fires)
 *    *             -> Disconnected  (Disconnect() or Deinitialize)
 */
UENUM(BlueprintType)
enum class EEstuarySessionState : uint8
{
    /** No active socket; no reconnect scheduled. Initial / post-Disconnect state. */
    Disconnected    UMETA(DisplayName = "Disconnected"),

    /** Socket opening; authenticate emitted; awaiting session_info from server. */
    Connecting      UMETA(DisplayName = "Connecting"),

    /** session_info received; session is live and ready for text/audio/animation. */
    Connected       UMETA(DisplayName = "Connected"),

    /** Transport closed unexpectedly; exponential backoff timer is running. */
    Reconnecting    UMETA(DisplayName = "Reconnecting"),

    /** Terminal failure (auth_error, session_rejected, or unrecoverable transport error).
     *  Consumer must call Connect() again with a corrected payload to recover. */
    Error           UMETA(DisplayName = "Error")
};

// ---------------------------------------------------------------------------
// FEstuaryAuthPayload — Client -> Server "authenticate" event payload.
// Wire contract: SDK_CONTRACT.md §Connection & Authentication.
// Fields map 1-to-1 to JSON keys (FJsonObjectConverter uses camelCase by default;
// Plan 02 bridge overrides key names via the converter flags or manual JSON build
// to stay snake_case as the contract requires).
// ---------------------------------------------------------------------------

/** Authentication payload sent to the Estuary gateway on every Socket.IO connect.
 *
 *  Populate this struct and pass it to UEstuarySubsystem::Connect() or the
 *  latent UEstuaryConnectAction::Connect Blueprint node.
 *
 *  All string fields default to empty; AudioSampleRate defaults to 24000 Hz
 *  (server preferred). Set bEnableAnimation=true only if the target character
 *  has an Audio2Face pipeline enabled and you intend to drive ARKit-52 lipsync.
 *  When bEnableAnimation is true, AudioSampleRate is coerced to 16000 by the
 *  server's A2F gate (Phase 6 will enforce this on the client side too).
 */
USTRUCT(BlueprintType)
struct ESTUARY_API FEstuaryAuthPayload
{
    GENERATED_BODY()

    /** Developer API key (est_...). Never logged in plain text — bridge masks to est_***. */
    UPROPERTY(BlueprintReadWrite, Category = "Estuary|Auth",
              meta = (DisplayName = "API Key", ToolTip = "Estuary developer API key (est_...). Treated as a secret; never included in plain-text logs."))
    FString ApiKey;

    /** Character ID to connect to. Must be a valid character owned by the API key. */
    UPROPERTY(BlueprintReadWrite, Category = "Estuary|Auth",
              meta = (DisplayName = "Character ID", ToolTip = "UUID of the Estuary character to connect to."))
    FString CharacterId;

    /** Opaque player identifier. Scopes the session and memory to this player. */
    UPROPERTY(BlueprintReadWrite, Category = "Estuary|Auth",
              meta = (DisplayName = "Player ID", ToolTip = "Opaque player identifier. Scopes session and long-term memory."))
    FString PlayerId;

    /** Preferred TTS playback sample rate in Hz. Default 24000. Use 16000 when
     *  enabling animation (A2F gate). Changing this affects audio quality — do not
     *  lower without a platform-specific reason. */
    UPROPERTY(BlueprintReadWrite, Category = "Estuary|Auth",
              meta = (DisplayName = "Audio Sample Rate", ToolTip = "TTS output sample rate in Hz. Default 24000; use 16000 when bEnableAnimation is true."))
    int32 AudioSampleRate = 24000;

    /** Opt in to receive bot_animation ARKit-52 blendshape frames at 30 fps.
     *  Requires server-side ENABLE_A2F=true and AudioSampleRate=16000. */
    UPROPERTY(BlueprintReadWrite, Category = "Estuary|Auth",
              meta = (DisplayName = "Enable Animation", ToolTip = "Set true to receive bot_animation ARKit-52 lipsync frames. Requires AudioSampleRate=16000 and server ENABLE_A2F=true."))
    bool bEnableAnimation = false;

    /** Opt in to receive bot_pose GestureSync-driven body-animation frames at 30 fps.
     *  Requires server-side GestureSync inference enabled, the character's
     *  animation_capabilities.body.enabled flag, and AudioSampleRate=16000
     *  (HuBERT audio gate on the GestureSync inference path).
     *  Mirrors bEnableAnimation but for the body-animation stream (Phase 4 —
     *  body_animation_stream contract; sequence 0..N then sequence==-1 terminator). */
    UPROPERTY(BlueprintReadWrite, Category = "Estuary|Auth",
              meta = (DisplayName = "Enable Body Animation",
                      ToolTip = "Set true to receive bot_pose GestureSync body-animation frames. Requires server GestureSync inference + per-character animation_capabilities.body.enabled. AudioSampleRate must be 16000 (HuBERT gate)."))
    bool bEnableBodyAnimation = false;

    /** SDK version string sent as a diagnostic field. Server ignores unknown fields.
     *  Populated automatically by the subsystem if left empty. */
    UPROPERTY(BlueprintReadWrite, Category = "Estuary|Auth",
              meta = (DisplayName = "Client Version", ToolTip = "SDK/client version string. Diagnostic only — server ignores unknown fields."))
    FString ClientVersion;

    /** Platform identifier (e.g. \"UnrealEngine/5.5 Win64\"). Diagnostic only. */
    UPROPERTY(BlueprintReadWrite, Category = "Estuary|Auth",
              meta = (DisplayName = "Platform", ToolTip = "Platform string for diagnostics. Example: UnrealEngine/5.5 Win64."))
    FString Platform;
};

// ---------------------------------------------------------------------------
// FEstuaryAnimationModalityCapability — one modality of
// session_info.animation_capabilities (face or body).
// Wire contract: SDK_CONTRACT.md §session_info.animation_capabilities.
// ---------------------------------------------------------------------------

/** Runtime capability gate for a single animation modality (face or body).
 *
 *  This is the server's connect-time AND-gate result. When bActive is true the
 *  server WILL emit this modality's frames for the session (face → bot_animation;
 *  body → bot_pose). When false it will NOT, regardless of the client's opt-in —
 *  e.g. the global provider is disabled, the character lacks the capability, or
 *  the negotiated audio_sample_rate failed the gate (body requires 16000). */
USTRUCT(BlueprintType)
struct ESTUARY_API FEstuaryAnimationModalityCapability
{
    GENERATED_BODY()

    /** True if the server will emit this modality's animation frames for the session. */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Session",
              meta = (DisplayName = "Active",
                      ToolTip = "Runtime AND-gate result. If true, the server WILL emit this modality's animation frames for the session."))
    bool bActive = false;

    /** Active provider name for this modality (e.g. "a2f" for face, "sarah" for body). Empty when inactive. */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Session",
              meta = (DisplayName = "Provider",
                      ToolTip = "Active provider name for this modality (e.g. a2f / sarah). Empty when inactive."))
    FString Provider;

    /** Wire rig identifier for this modality. Body reports "mhr63" (MHR 63-joint
     *  GestureSync wire). Empty when the server predates the rig field or the
     *  modality doesn't carry one (face). The body receiver hard-checks
     *  Body.Rig — a mismatch disables the receiver with a loud Error log
     *  (no silent frame drops between mixed client/server versions). */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Session",
              meta = (DisplayName = "Rig",
                      ToolTip = "Wire rig identifier (body: \"mhr63\"). Empty on older servers or for modalities without a rig."))
    FString Rig;
};

// ---------------------------------------------------------------------------
// FEstuaryAnimationCapabilities — session_info.animation_capabilities block.
// Wire contract: SDK_CONTRACT.md §session_info.animation_capabilities.
// ---------------------------------------------------------------------------

/** Connect-time animation capability gates reported in session_info.
 *
 *  These reflect the server's runtime AND-gates at connect time; mid-session
 *  operator changes are NOT propagated (an UPDATE only affects new sessions).
 *  Older servers omit the block entirely — check bPresent before trusting the
 *  modality flags; when absent, consumers should fall back to their auth opt-in.
 *  Unknown future modalities (e.g. gaze) are tolerated and ignored (forward-compat). */
USTRUCT(BlueprintType)
struct ESTUARY_API FEstuaryAnimationCapabilities
{
    GENERATED_BODY()

    /** Face (ARKit-52 / A2F) modality gate. */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Session",
              meta = (DisplayName = "Face"))
    FEstuaryAnimationModalityCapability Face;

    /** Body (MHR 63-joint GestureSync) modality gate. Body.Rig carries the wire rig id ("mhr63"). */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Session",
              meta = (DisplayName = "Body"))
    FEstuaryAnimationModalityCapability Body;

    /** True if the server actually included an animation_capabilities block in
     *  session_info. False on older servers — treat the modality flags as unknown. */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Session",
              meta = (DisplayName = "Present",
                      ToolTip = "True if the server reported animation_capabilities. If false, the Face/Body flags are unknown (older server)."))
    bool bPresent = false;
};

// ---------------------------------------------------------------------------
// FEstuarySessionInfo — Server -> Client "session_info" event payload.
// Wire contract: SDK_CONTRACT.md §Server → Client Session Events.
// ---------------------------------------------------------------------------

/** Session metadata received from the gateway after successful authentication.
 *
 *  Available on UEstuarySubsystem::GetSessionInfo() once OnSessionConnected fires.
 *  All fields are read-only from Blueprint; they are set by the subsystem on receipt.
 */
USTRUCT(BlueprintType)
struct ESTUARY_API FEstuarySessionInfo
{
    GENERATED_BODY()

    /** Unique session UUID. Identifies this specific WebSocket session on the gateway. */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Session",
              meta = (DisplayName = "Session ID", ToolTip = "Unique session UUID assigned by the gateway."))
    FString SessionId;

    /** Conversation UUID. Groups all messages in this conversational exchange.
     *  Use this for memory queries and bot_response correlation. */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Session",
              meta = (DisplayName = "Conversation ID", ToolTip = "Conversation UUID grouping all messages in this exchange."))
    FString ConversationId;

    /** Character ID confirmed by the server — should match the auth payload's CharacterId. */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Session",
              meta = (DisplayName = "Character ID", ToolTip = "Character UUID confirmed by the server."))
    FString CharacterId;

    /** Player ID confirmed by the server — should match the auth payload's PlayerId. */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Session",
              meta = (DisplayName = "Player ID", ToolTip = "Player ID confirmed by the server."))
    FString PlayerId;

    /** Connect-time animation capability gates (face/body) reported by the server.
     *  If AnimationCapabilities.Body.bActive is false the server will NOT emit
     *  bot_pose for this session (likewise Face/bot_animation). See
     *  SDK_CONTRACT.md §session_info.animation_capabilities. AnimationCapabilities.bPresent
     *  is false on older servers that omit the block. */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Session",
              meta = (DisplayName = "Animation Capabilities",
                      ToolTip = "Connect-time face/body animation capability gates. If Body.Active is false the server will not emit bot_pose. Present=false on older servers."))
    FEstuaryAnimationCapabilities AnimationCapabilities;
};

// ---------------------------------------------------------------------------
// FEstuaryAuthError — Server -> Client "auth_error" event payload.
// Wire contract: SDK_CONTRACT.md §Server → Client Session Events.
// ---------------------------------------------------------------------------

/** Authentication failure response from the gateway.
 *
 *  Received when the api_key, character_id, or player_id in FEstuaryAuthPayload
 *  is rejected. The session cannot proceed; consumer must correct the payload
 *  and call Connect() again.
 */
USTRUCT(BlueprintType)
struct ESTUARY_API FEstuaryAuthError
{
    GENERATED_BODY()

    /** Human-readable error code or message from the gateway (e.g., "invalid_api_key"). */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Error",
              meta = (DisplayName = "Error", ToolTip = "Error code or message from the gateway explaining the auth failure."))
    FString Error;
};

// ---------------------------------------------------------------------------
// FEstuarySessionRejected — Server -> Client "session_rejected" event payload.
// Wire contract: SDK_CONTRACT.md §Server → Client Session Events.
// ---------------------------------------------------------------------------

/** Server-side policy rejection (e.g., concurrent session cap exceeded).
 *
 *  Distinct from FEstuaryAuthError: the credentials were valid, but a server-side
 *  policy prevents opening a new session right now.
 *  The gateway disconnects immediately after sending this event.
 */
USTRUCT(BlueprintType)
struct ESTUARY_API FEstuarySessionRejected
{
    GENERATED_BODY()

    /** Rejection reason code. Example: "concurrent_limit" (too many active sessions
     *  for this share token). */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Error",
              meta = (DisplayName = "Reason", ToolTip = "Rejection reason code. E.g. concurrent_limit — wait for another device to disconnect."))
    FString Reason;

    /** Informational cap value — the limit that was exceeded (e.g., max concurrent sessions). */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Error",
              meta = (DisplayName = "Cap", ToolTip = "The policy cap that was exceeded (informational)."))
    int32 Cap = 0;

    /** ID of the share token whose policy triggered this rejection. */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Error",
              meta = (DisplayName = "Share Token ID", ToolTip = "ID of the share token whose concurrent-session policy triggered this rejection."))
    FString ShareTokenId;
};

// ---------------------------------------------------------------------------
// FEstuaryError — Server -> Client `error` event payload.
// Wire contract: SDK_CONTRACT.md §error — `{message: string}`.
//
// Non-terminal: the gateway does NOT disconnect after `error` (distinct from
// auth_error / session_rejected / quota_exceeded). Surfaced for diagnostics —
// e.g. "Not authenticated. Send 'authenticate' event first." when a `text` /
// `say_line` arrives before authentication (backend sdk_handlers.py).
// ---------------------------------------------------------------------------

/** Non-terminal server error notification. The session remains open. */
USTRUCT(BlueprintType)
struct ESTUARY_API FEstuaryError
{
    GENERATED_BODY()

    /** Human-readable error message from the gateway. Wire key: "message". */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Error",
              meta = (DisplayName = "Message", ToolTip = "Human-readable error message from the gateway. The session is NOT terminated."))
    FString Message;
};

// ---------------------------------------------------------------------------
// FEstuaryQuotaExceeded — Server -> Client `quota_exceeded` event payload.
// Wire contract: SDK_CONTRACT.md §quota_exceeded.
//
// Backend (sdk_handlers.py) emits TWO variants of this event:
//   1. quota variant     — {error, message, current, limit, remaining, tier}
//   2. share-limit variant — {error, message}  (numeric fields OMITTED)
// The deserializer MUST tolerate the missing numeric fields (defaults below).
// The gateway DISCONNECTS immediately after emitting either variant — do not
// assume further events follow.
//
// Kept DISTINCT from FEstuaryError on purpose: the numeric quota fields are the
// point of this event (a consumer can show "12 / 10 used, 0 remaining (free)").
// ---------------------------------------------------------------------------

/** Quota / share-interaction-limit rejection. The gateway disconnects right after. */
USTRUCT(BlueprintType)
struct ESTUARY_API FEstuaryQuotaExceeded
{
    GENERATED_BODY()

    /** Machine-readable error code. Wire key: "error".
     *  Known codes: "quota_exceeded" (numeric fields present) or
     *  "share_limit_exceeded" (numeric fields omitted). */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Error",
              meta = (DisplayName = "Error Code", ToolTip = "Machine-readable code: quota_exceeded or share_limit_exceeded."))
    FString ErrorCode;

    /** Human-readable message. Wire key: "message". */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Error",
              meta = (DisplayName = "Message", ToolTip = "Human-readable quota message from the gateway."))
    FString Message;

    /** Current usage count. Wire key: "current". 0 when omitted (share-limit variant). */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Error",
              meta = (DisplayName = "Current", ToolTip = "Current usage count. 0 when the server omits it (share-limit variant)."))
    int32 Current = 0;

    /** Quota limit. Wire key: "limit". 0 when omitted (share-limit variant). */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Error",
              meta = (DisplayName = "Limit", ToolTip = "Quota limit. 0 when the server omits it (share-limit variant)."))
    int32 Limit = 0;

    /** Remaining quota. Wire key: "remaining". 0 when omitted (share-limit variant). */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Error",
              meta = (DisplayName = "Remaining", ToolTip = "Remaining quota. 0 when the server omits it (share-limit variant)."))
    int32 Remaining = 0;

    /** Subscription tier label. Wire key: "tier". Empty when omitted (share-limit variant). */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Error",
              meta = (DisplayName = "Tier", ToolTip = "Subscription tier (e.g. free / pro). Empty when the server omits it (share-limit variant)."))
    FString Tier;
};

// ---------------------------------------------------------------------------
// FEstuarySessionTimeout — Server -> Client `session_timeout` event payload.
// Wire contract: SDK_CONTRACT.md §session_timeout.
//
// A voice-ACTIVE session had no conversation activity for TimeoutSeconds.
// The gateway FORCE-CLOSES the session immediately after emitting this (socket
// disconnected, LiveKit room deleted server-side — billing stops). The
// subsystem suppresses its auto-reconnect for the disconnect that follows;
// resuming requires an explicit Connect() (user intent), which starts a NEW
// session.
// ---------------------------------------------------------------------------

/** Server idle reap notification. The gateway disconnects right after; the SDK
 *  will NOT auto-reconnect (that would re-establish billed voice resources in a loop). */
USTRUCT(BlueprintType)
struct ESTUARY_API FEstuarySessionTimeout
{
    GENERATED_BODY()

    /** Machine-readable reason. Wire key: "reason". Currently always "inactivity". */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Session",
              meta = (DisplayName = "Reason", ToolTip = "Why the server ended the session. Currently always 'inactivity'."))
    FString Reason;

    /** How long the session was idle when reaped, in seconds. Wire key: "idle_seconds". */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Session",
              meta = (DisplayName = "Idle Seconds", ToolTip = "Observed idle time in seconds when the server reaped the session."))
    int32 IdleSeconds = 0;

    /** The server's configured idle timeout, in seconds. Wire key: "timeout_seconds". */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Session",
              meta = (DisplayName = "Timeout Seconds", ToolTip = "Server idle-timeout threshold in seconds (SESSION_IDLE_TIMEOUT_S)."))
    int32 TimeoutSeconds = 0;
};

// ---------------------------------------------------------------------------
// FEstuaryVoiceTimeout — Server -> Client `voice_timeout` event payload.
// Wire contract: SDK_CONTRACT.md §voice_timeout.
//
// A voice-ACTIVE session heard no USER SPEECH for TimeoutSeconds while other
// activity (text chat) kept the session alive. The server released ONLY the
// voice resources (STT closed, LiveKit room deleted server-side) — the socket
// STAYS CONNECTED and text continues. Opposite of session_timeout: no
// disconnect follows, so this must never feed the reconnect-suppression path.
// Recommended UX: auto-mute illusion — keep the call UI open, show the mic as
// muted, and start a FRESH voice session when the user unmutes.
// ---------------------------------------------------------------------------

/** Voice-lane idle release. Socket stays connected; text chat continues.
 *  Local voice state has already been released by the SDK when this fires. */
USTRUCT(BlueprintType)
struct ESTUARY_API FEstuaryVoiceTimeout
{
    GENERATED_BODY()

    /** Machine-readable reason. Wire key: "reason". Currently always "voice_inactivity". */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Voice",
              meta = (DisplayName = "Reason", ToolTip = "Why the server released voice resources. Currently always 'voice_inactivity'."))
    FString Reason;

    /** How long the voice lane was idle when released, in seconds. Wire key: "idle_seconds". */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Voice",
              meta = (DisplayName = "Idle Seconds", ToolTip = "Observed voice-idle time in seconds (no user speech)."))
    int32 IdleSeconds = 0;

    /** The server's configured voice-idle timeout, in seconds. Wire key: "timeout_seconds". */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Voice",
              meta = (DisplayName = "Timeout Seconds", ToolTip = "Server voice-idle threshold in seconds (VOICE_IDLE_TIMEOUT_S)."))
    int32 TimeoutSeconds = 0;
};

// ---------------------------------------------------------------------------
// FEstuaryMemoryData — one item of `memory_updated.new_memories[]`.
// Wire contract: SDK_CONTRACT.md §Shared Data Models / MemoryData.
//
// CRITICAL: unlike every other wire event in this SDK, the items in
// `new_memories[]` use camelCase keys (they mirror the server's
// Memory.to_dict()). The bridge parses them with the exact camelCase wire keys
// listed below.
//
// Pragmatic subset: this struct carries the high-value fields. The following
// Memory.to_dict() keys are intentionally OMITTED (rarely useful to a realtime
// game client; they are available via the REST memory API):
//   userId, agentId, playerId  — redundant with the enclosing MemoryUpdatedEvent
//                                 (agent_id / player_id) for the common single-
//                                 player case.
//   sourceConversationId       — conversation correlation belongs at the event
//                                 level (conversation_id is already there).
//   lastAccessedAt, extractedAt, sourceMessageTimestamp
//                              — bookkeeping timestamps; CreatedAt/UpdatedAt cover
//                                 the common "is this new" need.
//   source                     — provenance ("text_chat"/"simulation"); diagnostic.
// (sourceMessageTimestamp is additionally NOT in the SDK_CONTRACT MemoryData
//  model — the live server emits it; tolerated as an unknown field by the bridge.)
// ---------------------------------------------------------------------------

/** A single extracted memory from `memory_updated.new_memories[]` (camelCase wire keys). */
USTRUCT(BlueprintType)
struct ESTUARY_API FEstuaryMemoryData
{
    GENERATED_BODY()

    /** Memory UUID. Wire key: "id". */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Memory",
              meta = (DisplayName = "Id", ToolTip = "Memory UUID."))
    FString Id;

    /** Memory content text. Wire key: "content". */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Memory",
              meta = (DisplayName = "Content", ToolTip = "Extracted memory content."))
    FString Content;

    /** Memory category. Wire key: "memoryType".
     *  E.g. fact / preference / relationship / event / emotional_state /
     *  correction / character_self / spatial_change. */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Memory",
              meta = (DisplayName = "Memory Type", ToolTip = "Memory category (fact / preference / relationship / event / ...)."))
    FString MemoryType;

    /** Extraction confidence 0..1. Wire key: "confidence". */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Memory",
              meta = (DisplayName = "Confidence", ToolTip = "Extraction confidence in [0,1]."))
    float Confidence = 0.0f;

    /** Lifecycle status. Wire key: "status".
     *  E.g. active / superseded / decayed / deleted. */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Memory",
              meta = (DisplayName = "Status", ToolTip = "Lifecycle status (active / superseded / decayed / deleted)."))
    FString Status;

    /** Primary topic label. Wire key: "topic". */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Memory",
              meta = (DisplayName = "Topic", ToolTip = "Primary topic (Work, Family, Health, ...)."))
    FString Topic;

    /** Source quote the memory was extracted from. Wire key: "sourceQuote". */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Memory",
              meta = (DisplayName = "Source Quote", ToolTip = "Verbatim quote the memory was extracted from."))
    FString SourceQuote;

    /** Additional related topics. Wire key: "secondaryTopics" (array of strings). */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Memory",
              meta = (DisplayName = "Secondary Topics", ToolTip = "Additional related topic labels."))
    TArray<FString> SecondaryTopics;

    /** Access count. Wire key: "accessCount". */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Memory",
              meta = (DisplayName = "Access Count", ToolTip = "Number of times this memory has been accessed."))
    int32 AccessCount = 0;

    /** Creation timestamp (ISO-8601). Wire key: "createdAt". Empty when null. */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Memory",
              meta = (DisplayName = "Created At", ToolTip = "ISO-8601 creation timestamp. Empty when the server sent null."))
    FString CreatedAt;

    /** Last-update timestamp (ISO-8601). Wire key: "updatedAt". Empty when null. */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Memory",
              meta = (DisplayName = "Updated At", ToolTip = "ISO-8601 last-update timestamp. Empty when the server sent null."))
    FString UpdatedAt;
};

// ---------------------------------------------------------------------------
// FEstuaryMemoryUpdated — Server -> Client `memory_updated` event payload.
// Wire contract: SDK_CONTRACT.md §memory_push / MemoryUpdatedEvent.
//
// TOP-LEVEL keys are snake_case (agent_id, player_id, memories_extracted, ...),
// but NewMemories items (FEstuaryMemoryData) use camelCase — see that struct.
// Pushed after background memory extraction completes; dropped if disconnected.
// ---------------------------------------------------------------------------

/** Notification that new memories were extracted after a conversation ended. */
USTRUCT(BlueprintType)
struct ESTUARY_API FEstuaryMemoryUpdated
{
    GENERATED_BODY()

    /** Agent (character) UUID. Wire key: "agent_id". */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Memory",
              meta = (DisplayName = "Agent ID", ToolTip = "Character UUID the memories belong to."))
    FString AgentId;

    /** Player UUID. Wire key: "player_id". */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Memory",
              meta = (DisplayName = "Player ID", ToolTip = "Player the memories are scoped to."))
    FString PlayerId;

    /** Count of memories extracted this round. Wire key: "memories_extracted". */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Memory",
              meta = (DisplayName = "Memories Extracted", ToolTip = "Number of memories extracted this round."))
    int32 MemoriesExtracted = 0;

    /** Count of facts extracted this round. Wire key: "facts_extracted". */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Memory",
              meta = (DisplayName = "Facts Extracted", ToolTip = "Number of facts extracted this round."))
    int32 FactsExtracted = 0;

    /** Source conversation UUID. Wire key: "conversation_id". */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Memory",
              meta = (DisplayName = "Conversation ID", ToolTip = "Conversation the memories were extracted from."))
    FString ConversationId;

    /** Newly extracted memories. Wire key: "new_memories" (array; items use camelCase keys). */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Memory",
              meta = (DisplayName = "New Memories", ToolTip = "The newly extracted memories (camelCase item keys)."))
    TArray<FEstuaryMemoryData> NewMemories;

    /** Server emit timestamp (ISO-8601). Wire key: "timestamp". */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Memory",
              meta = (DisplayName = "Timestamp", ToolTip = "ISO-8601 server emit timestamp."))
    FString Timestamp;
};

// ---------------------------------------------------------------------------
// FEstuaryUserText — Client -> Server `text` event payload.
// Wire contract: SDK_CONTRACT.md §text_chat.
//
// The SDK_CONTRACT `text` wire payload is {text, textOnly?}. This USTRUCT
// carries two extra CLIENT-SIDE correlation fields (MessageId, CharacterId)
// that never cross the wire via Plan 03-02's emit helper but ARE surfaced to
// Blueprint so consumers can correlate outbound messages to OnBotResponse
// broadcasts in multi-character scenes (TEXT-06).
// ---------------------------------------------------------------------------

/** Outbound user text message. Populate via UEstuaryCharacterComponent::SendText.
 *
 *  MessageId is generated client-side with FGuid::NewGuid().ToString(EGuidFormats::Digits)
 *  in Plan 03-04 (matches ts-sdk convention: lowercase hex, no dashes). CharacterId is
 *  set by the component to its owning AEstuaryCharacter's Id so multi-character games
 *  can correlate streaming bot_response chunks back to the right component.
 */
USTRUCT(BlueprintType)
struct ESTUARY_API FEstuaryUserText
{
    GENERATED_BODY()

    /** The text to send to the bot. Passed through verbatim to the wire `text.text` field. */
    UPROPERTY(BlueprintReadWrite, Category = "Estuary|Text",
              meta = (DisplayName = "Text", ToolTip = "User text to send. Empty string is allowed but typically a no-op."))
    FString Text;

    /** Client-generated message correlation ID. Set by SendText; used to route bot_response
     *  and interrupt back to the originating AEstuaryCharacter. Not transmitted as a wire
     *  field on `text` — the server uses session scope — but echoed in bot_response chunks. */
    UPROPERTY(BlueprintReadWrite, Category = "Estuary|Text",
              meta = (DisplayName = "Message ID", ToolTip = "Client-generated correlation UUID (digits-only). Matches bot_response.message_id for this turn."))
    FString MessageId;

    /** Owning character's client-side Id (FGuid::ToString(EGuidFormats::Digits)).
     *  Allows multi-character scenes to route streaming responses without cross-talk. */
    UPROPERTY(BlueprintReadWrite, Category = "Estuary|Text",
              meta = (DisplayName = "Character ID", ToolTip = "Client-side character correlation ID (not the auth character_id)."))
    FString CharacterId;

    /** Mirror of the wire `textOnly` flag. When true, server suppresses bot_voice emission
     *  for this turn and returns only bot_response. Defaults to false. */
    UPROPERTY(BlueprintReadWrite, Category = "Estuary|Text",
              meta = (DisplayName = "Text Only", ToolTip = "When true, server returns bot_response chunks only and suppresses bot_voice audio."))
    bool bTextOnly = false;
};

// ---------------------------------------------------------------------------
// FEstuaryBotResponse — Server -> Client `bot_response` event payload.
// Wire contract: SDK_CONTRACT.md §text_chat / §Shared Data Models.
//
// Fields map to snake_case wire keys:
//   Text            <- text
//   Partial         <- partial
//   bIsFinal        <- is_final
//   MessageId       <- message_id
//   ChunkIndex      <- chunk_index
//   bIsInterjection <- is_interjection
// CharacterId is a CLIENT-ADDED routing field (Plan 03-03 subsystem copies it in
// from FEstuarySessionInfo on dispatch); NOT present on the wire.
// ---------------------------------------------------------------------------

/** A single streamed bot_response chunk. Fires once per chunk plus a final is_final=true
 *  emission. Aggregate Text across chunks for the full response; `Partial` is the
 *  server-maintained running partial string (may be empty on some server configurations).
 *
 *  Zero-safe defaults (Pitfall 19): missing fields deserialize to empty/false/-1 rather
 *  than producing a parse failure. ChunkIndex defaults to -1 to sentinel "unset" so a
 *  caller can tell the server genuinely emitted a chunk_index of 0 versus omitted the field.
 */
USTRUCT(BlueprintType)
struct ESTUARY_API FEstuaryBotResponse
{
    GENERATED_BODY()

    /** The delta text for this chunk. Concatenate across chunks to build the full response. */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Bot",
              meta = (DisplayName = "Text", ToolTip = "Delta text for this chunk. Concatenate across chunks to build the full response."))
    FString Text;

    /** Server-maintained running partial string. May be empty depending on server config. */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Bot",
              meta = (DisplayName = "Partial", ToolTip = "Server-maintained running partial string (may be empty)."))
    FString Partial;

    /** Correlation ID — matches the FEstuaryUserText.MessageId that originated this turn. */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Bot",
              meta = (DisplayName = "Message ID", ToolTip = "Server-echoed message_id. Matches the originating user_text's MessageId."))
    FString MessageId;

    /** Client-side character routing ID — populated by the subsystem on dispatch.
     *  Never sourced from the wire (bot_response has no character_id wire field). */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Bot",
              meta = (DisplayName = "Character ID", ToolTip = "Client-side character correlation ID (populated by the subsystem on dispatch)."))
    FString CharacterId;

    /** 0-based chunk counter within the message. -1 = unset / not present on the wire. */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Bot",
              meta = (DisplayName = "Chunk Index", ToolTip = "Zero-based chunk counter. -1 sentinel when field missing from wire."))
    int32 ChunkIndex = -1;

    /** True on the final chunk of a message. Consumer should not expect further chunks with
     *  this MessageId after is_final=true fires. */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Bot",
              meta = (DisplayName = "Is Final", ToolTip = "True on the final chunk. No further bot_response chunks with this MessageId after this fires."))
    bool bIsFinal = false;

    /** True when the bot interjected (e.g., a backchannel response) rather than answering a
     *  user turn. May be false for say_line-sourced responses per SDK_CONTRACT. */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Bot",
              meta = (DisplayName = "Is Interjection", ToolTip = "True for backchannel/interjection responses rather than normal turn answers."))
    bool bIsInterjection = false;
};

// ---------------------------------------------------------------------------
// FEstuaryInterruptPayload — bidirectional: client_interrupt emit, interrupt receive.
// Wire contract: SDK_CONTRACT.md §interrupts.
//
// Client -> Server `client_interrupt` carries {message_id?}. Client emits this
// to abort an in-flight response.
//
// Server -> Client `interrupt` carries {message_id?, reason?, interrupted_at?}.
// Server acknowledges the abort (or self-initiates one, e.g., on new user turn).
// CharacterId is a CLIENT-ADDED field populated by the subsystem on receive —
// not on the wire — so per-character dispatch works (TEXT-06).
// ---------------------------------------------------------------------------

/** Interrupt payload — shared by the outbound `client_interrupt` emit and the inbound
 *  `interrupt` receive. Reason / InterruptedAt are server-only fields; empty on outbound. */
USTRUCT(BlueprintType)
struct ESTUARY_API FEstuaryInterruptPayload
{
    GENERATED_BODY()

    /** Correlation ID for the message being interrupted. May be empty if the client intends
     *  to interrupt whatever is currently streaming (server interprets blank as "current"). */
    UPROPERTY(BlueprintReadWrite, Category = "Estuary|Interrupt",
              meta = (DisplayName = "Message ID", ToolTip = "MessageId being interrupted. Empty = interrupt current in-flight message."))
    FString MessageId;

    /** Server-provided reason code (e.g., "client_request", "new_turn"). Empty on outbound. */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Interrupt",
              meta = (DisplayName = "Reason", ToolTip = "Server reason code on inbound interrupt. Empty on outbound client_interrupt emit."))
    FString Reason;

    /** Server wall-clock timestamp of the interrupt, ISO-8601 string. Empty on outbound. */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Interrupt",
              meta = (DisplayName = "Interrupted At", ToolTip = "ISO-8601 timestamp when server processed the interrupt. Empty on outbound."))
    FString InterruptedAt;

    /** Client-side character correlation ID — populated by the subsystem on dispatch. */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Interrupt",
              meta = (DisplayName = "Character ID", ToolTip = "Client-side character correlation ID (populated on dispatch; empty on outbound)."))
    FString CharacterId;
};

// ---------------------------------------------------------------------------
// FEstuaryPreferences — Client -> Server `update_preferences` event payload.
// Wire contract: SDK_CONTRACT.md §preferences.
//
// Canonical wire shape: {enableVisionAcknowledgment?: boolean}. Note the server
// uses camelCase here (the rest of the contract is snake_case). The emit helper
// in Plan 03-02 MUST serialize the field as `enableVisionAcknowledgment`.
//
// ExtraPreferences is a forward-compatibility bag (Pitfall 19 defense): lets
// Blueprint consumers set keys the SDK doesn't yet explicit-field without
// blocking on an SDK update. Keys are emitted verbatim (bridge camelCase-s /
// passes through as authored); values are strings (bridge parses bool/number
// only if server schema adds canonical typed fields later).
// ---------------------------------------------------------------------------

/** Session-level preference flags. Passed to UEstuaryCharacterComponent::UpdatePreferences
 *  to emit `update_preferences`. Server acknowledges by applying the new preferences to
 *  subsequent bot responses (no direct ack event in the contract — observability is via
 *  behavior change on subsequent turns). */
USTRUCT(BlueprintType)
struct ESTUARY_API FEstuaryPreferences
{
    GENERATED_BODY()

    /** Canonical contract flag: when true, the bot verbally acknowledges vision captures
     *  (e.g., "I can see you're holding a book"). Serialized as `enableVisionAcknowledgment`
     *  (camelCase on the wire, per SDK_CONTRACT §preferences). */
    UPROPERTY(BlueprintReadWrite, Category = "Estuary|Preferences",
              meta = (DisplayName = "Enable Vision Acknowledgment",
                      ToolTip = "Canonical vision-acknowledgment flag. Serialized as camelCase `enableVisionAcknowledgment` on the wire."))
    bool bEnableVisionAcknowledgment = false;

    /** Forward-compatibility bag for preference keys the SDK doesn't explicit-field yet.
     *  Keys are emitted verbatim; values are stringly-typed. Use sparingly — prefer adding
     *  explicit UPROPERTY fields once a preference becomes standard in SDK_CONTRACT. */
    UPROPERTY(BlueprintReadWrite, Category = "Estuary|Preferences",
              meta = (DisplayName = "Extra Preferences",
                      ToolTip = "Forward-compat key/value bag. Emitted verbatim alongside explicit fields."))
    TMap<FString, FString> ExtraPreferences;
};

// ---------------------------------------------------------------------------
// Voice / STT wire types — Phase 5 (MIC-03, MIC-04).
// SDK_CONTRACT.md §voice_websocket.
// ---------------------------------------------------------------------------

/** Typed handle for the 'start_voice' client->server event.
 *  Wire payload is null (no fields). Emit via UEstuarySubsystem::SendStartVoice
 *  before streaming audio chunks. */
USTRUCT(BlueprintType)
struct ESTUARY_API FEstuaryStartVoice
{
    GENERATED_BODY()
    // start_voice is emitted with no JSON payload (null). This struct is a typed
    // handle for Blueprint callers. No wire fields needed.
};

/** Payload for the 'stream_audio' client->server event.
 *  Carries one base64-encoded PCM16 16 kHz mono audio chunk per event. */
USTRUCT(BlueprintType)
struct ESTUARY_API FEstuaryStreamAudio
{
    GENERATED_BODY()

    /** Base64-encoded PCM16 16 kHz mono audio chunk. Wire key: "audio". */
    UPROPERTY(BlueprintReadWrite, Category = "Estuary|Voice",
              meta = (DisplayName = "Audio (Base64)",
                      ToolTip = "Base64-encoded PCM16 16 kHz mono chunk for stream_audio wire event."))
    FString Audio;
};

/** Typed handle for the 'stop_voice' client->server event.
 *  Wire payload is null (no fields). Emit via UEstuarySubsystem::SendStopVoice
 *  after the last audio chunk. */
USTRUCT(BlueprintType)
struct ESTUARY_API FEstuaryStopVoice
{
    GENERATED_BODY()
    // stop_voice is emitted with no JSON payload (null). Typed handle for Blueprint.
};

/** Server -> Client 'stt_response' payload.
 *  Fires once per partial STT result and once with bIsFinal=true on the final result.
 *  After is_final=true the server proceeds to the bot_response + bot_voice flow. */
USTRUCT(BlueprintType)
struct ESTUARY_API FEstuarySttResponse
{
    GENERATED_BODY()

    /** Transcript text for this partial or final STT result. Wire key: "text". */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Voice",
              meta = (DisplayName = "Text",
                      ToolTip = "STT transcript text. May be partial or final depending on bIsFinal."))
    FString Text;

    /** True on the final STT result for this turn. Wire key: "is_final". */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Voice",
              meta = (DisplayName = "Is Final",
                      ToolTip = "True when this is the final STT result. Server proceeds to bot_response after is_final=true."))
    bool bIsFinal = false;

    /** Client-side character routing ID — populated by the subsystem on dispatch. NOT on wire. */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Voice",
              meta = (DisplayName = "Character ID",
                      ToolTip = "Client-side character correlation ID (populated on dispatch; not a wire field)."))
    FString CharacterId;
};

// ---------------------------------------------------------------------------
// FEstuaryBotVoice — Server -> Client `bot_voice` event payload.
// Wire contract: SDK_CONTRACT.md §text_chat — bot_voice.
// Fields: audio, message_id, chunk_index, is_final.
//
// NOTE: There is NO sample_rate and NO character_id on the wire.
// Sample rate comes from UEstuaryDeveloperSettings.
// Character routing is handled by message_id matching in UEstuaryAudioPlayerComponent.
// ---------------------------------------------------------------------------

/** A single streaming TTS audio chunk from the gateway.
 *
 *  Audio is a base64-encoded PCM16 (int16_t) buffer. Decode in the consumer
 *  (UEstuaryAudioPlayerComponent::EnqueueBotVoice) before passing to the ring buffer.
 *  ChunkIndex defaults to -1 sentinel (field absent from wire means "unordered delivery").
 *  bIsFinal=true marks the last chunk for this MessageId; no further chunks will arrive.
 *
 *  Zero-safe defaults (Pitfall 19): missing fields deserialize to empty/false/-1 rather
 *  than producing a parse failure.
 */
USTRUCT(BlueprintType)
struct ESTUARY_API FEstuaryBotVoice
{
    GENERATED_BODY()

    /** Base64-encoded PCM16 (int16_t, mono) audio payload for this chunk.
     *  Wire key: "audio". Decode via FBase64::Decode before passing to ring buffer. */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Audio",
              meta = (DisplayName = "Audio", ToolTip = "Base64-encoded PCM16 mono audio chunk. Decode before playback."))
    FString Audio;

    /** Correlation ID matching the bot_response.message_id for this turn.
     *  Wire key: "message_id". Used by UEstuaryAudioPlayerComponent to filter stale-message chunks. */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Audio",
              meta = (DisplayName = "Message ID", ToolTip = "Correlation ID tying this voice chunk to a bot_response turn."))
    FString MessageId;

    /** 0-based index of this audio chunk within the message stream.
     *  Wire key: "chunk_index". -1 sentinel when field absent from wire. */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Audio",
              meta = (DisplayName = "Chunk Index", ToolTip = "Zero-based chunk counter for this message. -1 when field absent."))
    int32 ChunkIndex = -1;

    /** True on the final chunk for this MessageId; no further bot_voice chunks will arrive.
     *  Wire key: "is_final". */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Audio",
              meta = (DisplayName = "Is Final", ToolTip = "True on the last bot_voice chunk for this MessageId."))
    bool bIsFinal = false;
};

// ---------------------------------------------------------------------------
// FEstuaryAudioPlaybackComplete — Client -> Server `audio_playback_complete` emit.
// Wire contract: SDK_CONTRACT.md §audio_playback_tracking — audio_playback_complete.
//
// Emit when the tail of a message's audio drains from the ring buffer.
// message_id is optional per contract; emit empty string if unknown.
// ---------------------------------------------------------------------------

/** Playback-complete notification sent to the gateway when audio fully drains.
 *
 *  Emitted by UEstuaryAudioPlayerComponent::TickComponent when
 *  bFinalChunkReceived=true AND GetAvailableSamples()==0 for the current MessageId.
 *  The gateway uses this for latency analytics and optional follow-on logic.
 */
USTRUCT(BlueprintType)
struct ESTUARY_API FEstuaryAudioPlaybackComplete
{
    GENERATED_BODY()

    /** The message_id whose audio playback just finished.
     *  Wire key: "message_id". Optional per contract — emit empty string if unknown. */
    UPROPERTY(BlueprintReadWrite, Category = "Estuary|Audio",
              meta = (DisplayName = "Message ID", ToolTip = "The message_id whose audio has finished playing. Empty = unknown."))
    FString MessageId;
};

// ---------------------------------------------------------------------------
// FEstuaryBotAnimation — Server -> Client `bot_animation` event payload.
// Wire contract: SDK_CONTRACT.md §animation_stream.
//
// Fields (snake_case wire -> USTRUCT field):
//   message_id      -> MessageId   (utterance UUID)
//   sequence        -> FrameIndex  (0-based; -1 on terminator frame)
//   time_code_sec   -> TimeCodeSec (seconds from utterance start)
//   weights         -> Weights     (ARKit-52 name -> weight [0.0,1.0])
//   is_final        -> bIsFinal    (true only on terminator frame)
//   emit_epoch_ms   -> EmitEpochMs (worker wall-clock, diagnostics only)
//
// TERMINATOR FRAME: sequence == -1, weights == {}, is_final == true.
// The subsystem filters it before dispatch — AnimationReceiver never sees it.
// ---------------------------------------------------------------------------

USTRUCT(BlueprintType)
struct ESTUARY_API FEstuaryBotAnimation
{
    GENERATED_BODY()

    /** Utterance UUID — matches the owning bot_voice / bot_response message_id. */
    UPROPERTY(BlueprintReadWrite, Category="Estuary")
    FString MessageId;

    /**
     * 0-based frame counter within the utterance.
     * -1 signals the terminator frame — DO NOT pass to the animation buffer.
     */
    UPROPERTY(BlueprintReadWrite, Category="Estuary")
    int32 FrameIndex = -1;

    /**
     * Seconds from utterance start (time_code_sec from wire payload).
     * Monotonically increasing within a message_id. 0.0 on terminator.
     * This is the time base used by FAnimationFrameBuffer for audio-clock matching.
     */
    UPROPERTY(BlueprintReadWrite, Category="Estuary")
    float TimeCodeSec = 0.0f;

    /**
     * ARKit-52 blendshape name -> weight in [0.0, 1.0].
     * Empty map on the terminator frame (FrameIndex == -1 / bIsFinal == true).
     * TMap chosen over fixed-size TArray for diagnostic logging and future curve
     * extension (at 52 keys and 30 fps the GC overhead is negligible vs the
     * flexibility benefit — remap library lookups need the name as a key).
     */
    UPROPERTY(BlueprintReadWrite, Category="Estuary")
    TMap<FString, float> Weights;

    /**
     * True only on the terminator frame (FrameIndex == -1, Weights empty).
     * Clients MUST NOT treat the terminator as a renderable frame —
     * it signals the end of the animation stream for this message_id.
     */
    UPROPERTY(BlueprintReadWrite, Category="Estuary")
    bool bIsFinal = false;

    /**
     * Wall-clock millis at worker emit time — useful for RTT/drift diagnostics.
     * Not used for animation timing (use TimeCodeSec + audio clock instead).
     */
    UPROPERTY(BlueprintReadWrite, Category="Estuary")
    int64 EmitEpochMs = 0;

    /** Returns true if this frame is the terminator (must not be rendered). */
    bool IsTerminator() const { return FrameIndex == -1 && bIsFinal; }
};

// ---------------------------------------------------------------------------
// FEstuaryBotPoseBone / FEstuaryBotPose — Server -> Client `bot_pose` event payload.
// Wire contract: SDK_CONTRACT.md §body_animation_stream (MHR63 GestureSync).
//
// Bone count is exactly 63 — the MHR (Momentum Human Rig) wire subset in the
// canonical wire order (authority: SARAH models/rig.py via the generated
// EstuaryMHRRig.h): 0 root; 1-7 left leg; 8-14 right leg; 15-18 c_spine0..3;
// 19-22 r_clavicle..r_wrist; 23-39 right fingers; 40-43 l_clavicle..l_wrist;
// 44-60 left fingers; 61 c_neck; 62 c_head. Quats are LOCAL joint rotations in
// the MHR canonical frame (RH, +Y up, +Z forward), xyzw, w >= 0 per frame with
// per-bone cross-chunk sign-linking server-side. Identity != rest: at rest the
// locals equal MHR's A-pose rest local frames, NOT identity quats.
//
// Wire shape:
//   {
//     message_id: string,
//     sequence: number,                    // 0-based; -1 on terminator
//     time_code_sec: number,               // seconds from utterance start
//     fps: number,                         // always 30
//     bones: Array<{name: string, quat: [x,y,z,w]}>,    // 63 MHR wire bones, LOCAL quats
//     hips_world: [x,y,z],                 // root-position delta, meters, agent frame
//     emit_epoch_ms: number,
//     is_final: boolean                    // true only on terminator (sequence==-1, bones==[])
//   }
//
// TERMINATOR FRAME: sequence == -1, bones == [], is_final == true.
// The subsystem filters it before dispatch — body-animation receiver never sees it.
// ---------------------------------------------------------------------------

/** Single bone entry within a bot_pose frame.
 *  Maps to one element of SDK_CONTRACT.md `bot_pose.bones[]`:
 *    bones[i].name -> Name      (MHR wire bone name; one of the 63 names)
 *    bones[i].quat -> Quat      (FVector4 carrying [x, y, z, w] float32 quaternion)
 *
 *  Identity quaternion default (0,0,0,1) so an absent / malformed entry parses
 *  to a benign value rather than crashing the renderer. NOTE: on the MHR rig
 *  identity != rest — the rest pose is MHR's A-pose local frames. */
USTRUCT(BlueprintType)
struct ESTUARY_API FEstuaryBotPoseBone
{
    GENERATED_BODY()

    /** MHR wire bone name. One of the 63 canonical names in wire order
     *  (root, l_upleg, ..., c_head — authority: generated EstuaryMHRRig.h).
     *  Wire name == SK_GestureSyncSource skeleton bone name (generated FBX),
     *  so no client-side name remap exists. Wire key: bones[i].name. */
    UPROPERTY(BlueprintReadWrite, Category="Estuary|BodyAnimation",
              meta=(DisplayName="Name",
                    ToolTip="MHR wire bone name (one of the 63 names enumerated in SDK_CONTRACT body_animation_stream)."))
    FString Name;

    /** Quaternion as FVector4 with components ordered [x, y, z, w] (float32).
     *  LOCAL joint rotation in the MHR canonical frame (RH, +Y up, +Z forward);
     *  w >= 0 per frame with per-bone cross-chunk sign-linking server-side.
     *  Default is the identity quaternion (0,0,0,1) — NOT the rest pose.
     *  Wire key: bones[i].quat (4-element array). */
    UPROPERTY(BlueprintReadWrite, Category="Estuary|BodyAnimation",
              meta=(DisplayName="Quat (xyzw)",
                    ToolTip="LOCAL rotation quaternion [x, y, z, w] in the MHR canonical frame. Identity default = (0,0,0,1); identity != rest on the MHR rig."))
    FVector4 Quat = FVector4(0.0, 0.0, 0.0, 1.0);
};

/** A single GestureSync-driven body-pose frame (one frame in the bot_pose stream).
 *
 *  Field mapping (snake_case wire key -> USTRUCT field):
 *    message_id          -> MessageId          (utterance UUID)
 *    sequence            -> FrameIndex         (0-based; -1 on terminator)
 *    time_code_sec       -> TimeCodeSec        (seconds from utterance start)
 *    fps                 -> Fps                (always 30)
 *    bones               -> Bones              (63 MHR wire bones, LOCAL quats xyzw)
 *    hips_world          -> HipsWorld          (root-position delta, meters, agent frame)
 *    emit_epoch_ms       -> EmitEpochMs        (worker wall-clock; diagnostics only)
 *    is_final            -> bIsFinal           (true only on terminator frame)
 *
 *  HipsWorld is a pelvis-centred root delta in MHR canonical meters (Y
 *  oscillates ~0) — converted for UE by EstuaryBodyFrameTransform::ToUEHipsFromMHR
 *  in the body receiver before driving a Skeletal Mesh.
 */
USTRUCT(BlueprintType)
struct ESTUARY_API FEstuaryBotPose
{
    GENERATED_BODY()

    /** Utterance UUID — matches the owning bot_voice / bot_response message_id. */
    UPROPERTY(BlueprintReadWrite, Category="Estuary|BodyAnimation",
              meta=(DisplayName="Message ID",
                    ToolTip="Utterance UUID. Matches bot_voice.message_id / bot_response.message_id for this turn."))
    FString MessageId;

    /**
     * 0-based frame counter within the utterance.
     * -1 signals the terminator frame — DO NOT pass to the body-animation buffer.
     * Wire key: "sequence".
     */
    UPROPERTY(BlueprintReadWrite, Category="Estuary|BodyAnimation",
              meta=(DisplayName="Frame Index",
                    ToolTip="0-based frame counter within the utterance. -1 indicates the terminator frame."))
    int32 FrameIndex = -1;

    /**
     * Seconds from utterance start (time_code_sec from wire payload).
     * Monotonically increasing within a message_id. 0.0 on terminator.
     * This is the time base used to align body animation against the audio clock.
     */
    UPROPERTY(BlueprintReadWrite, Category="Estuary|BodyAnimation",
              meta=(DisplayName="Time Code (sec)",
                    ToolTip="Seconds from utterance start. Audio-clock-matched timing source for body-animation playback."))
    float TimeCodeSec = 0.0f;

    /** Frames-per-second hint from the server. Always 30 for v2.0.
     *  Wire key: "fps". */
    UPROPERTY(BlueprintReadWrite, Category="Estuary|BodyAnimation",
              meta=(DisplayName="FPS",
                    ToolTip="Server frame rate hint. Always 30 in v2.0."))
    int32 Fps = 30;

    /**
     * Exactly 63 MHR wire bones in the canonical wire order (authority: SARAH
     * models/rig.py via the generated EstuaryMHRRig.h — root, l_upleg, ...,
     * c_head). Empty array on the terminator frame (FrameIndex == -1 /
     * bIsFinal == true). Each entry carries a bone name and a LOCAL-rotation
     * [x, y, z, w] quaternion. The body receiver DROPS any non-terminator
     * frame whose count != 63 with a throttled Error (no legacy tolerance).
     */
    UPROPERTY(BlueprintReadWrite, Category="Estuary|BodyAnimation",
              meta=(DisplayName="Bones",
                    ToolTip="63 MHR wire-bone LOCAL rotations (MHR63 canonical wire order — see SDK_CONTRACT body_animation_stream)."))
    TArray<FEstuaryBotPoseBone> Bones;

    /**
     * Root-position delta in MHR canonical meters, agent frame (pelvis-centred;
     * Y oscillates ~0). Converted for UE by
     * EstuaryBodyFrameTransform::ToUEHipsFromMHR before driving a mesh.
     * Wire key: "hips_world" (3-element array of numbers).
     */
    UPROPERTY(BlueprintReadWrite, Category="Estuary|BodyAnimation",
              meta=(DisplayName="Hips World",
                    ToolTip="Root-position delta in MHR canonical meters (agent frame, pelvis-centred). Converted via EstuaryBodyFrameTransform::ToUEHipsFromMHR."))
    FVector HipsWorld = FVector::ZeroVector;

    /**
     * Wall-clock millis at worker emit time — useful for RTT/drift diagnostics.
     * Not used for body-animation timing (use TimeCodeSec + audio clock instead).
     */
    UPROPERTY(BlueprintReadWrite, Category="Estuary|BodyAnimation",
              meta=(DisplayName="Emit Epoch (ms)",
                    ToolTip="Worker wall-clock millis at emit time. Diagnostic only — do not use for animation timing."))
    int64 EmitEpochMs = 0;

    /**
     * True only on the terminator frame (FrameIndex == -1, Bones empty).
     * Clients MUST NOT treat the terminator as a renderable frame —
     * it signals the end of the body-animation stream for this message_id.
     */
    UPROPERTY(BlueprintReadWrite, Category="Estuary|BodyAnimation",
              meta=(DisplayName="Is Final",
                    ToolTip="True only on the terminator frame (sequence==-1, bones empty). Do not render."))
    bool bIsFinal = false;

    /** Returns true if this frame is the terminator (must not be rendered). */
    bool IsTerminator() const { return FrameIndex == -1 && bIsFinal; }
};

// ---------------------------------------------------------------------------
// EEstuaryFingerPolicy — body-receiver finger-driving policy (UE-09).
// Added early to keep the build green; semantics fully implemented in a later
// revision (which wires DefaultFingerPolicy in UEstuaryDeveloperSettings
// and adds the BindPose / IdleBreath / Statue branches in TickComponent).
//
// Now that GestureSync drives fingers (J=52 wire), `Driven` is the new default.
// ---------------------------------------------------------------------------

UENUM(BlueprintType)
enum class EEstuaryFingerPolicy : uint8
{
    /** (default) Live GestureSync frames drive fingers; bind-pose hold during pre-buffer. */
    Driven        UMETA(DisplayName = "Driven (default — GestureSync frames)"),

    /** Always rest pose. Debug / fallback for environments where finger animation is
     *  undesired (kiosk demos). */
    BindPose      UMETA(DisplayName = "Bind Pose (always rest)"),

    /** GestureSync frames PLUS subtle additive idle motion overlaid on the fingers. v2.0
     *  ships the enum value but the additive idle math is a stub (TODO: later revision). */
    IdleBreath    UMETA(DisplayName = "Idle Breath (Driven + additive idle [stub v2.0])"),

    /** Frozen at the last received frame indefinitely. Explicit "broken state"
     *  indicator for QA — never the default; surfaces "frames stopped flowing but
     *  client thought they were live" as a visible artifact rather than a silent freeze. */
    Statue        UMETA(DisplayName = "Statue (frozen at last frame — debug only)"),
};

// ---------------------------------------------------------------------------
// Camera / VLM wire types — Phase 7 (CAM-01..CAM-04).
// SDK_CONTRACT.md §vision_camera.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// EEstuaryCaptureSource — which camera the VLM "sees through" (CAM-07).
// Selected at runtime (e.g. a testbed toggle button). Governs the
// server-initiated camera_capture path (and the manual webcam/scene helpers).
// ---------------------------------------------------------------------------
UENUM(BlueprintType)
enum class EEstuaryCaptureSource : uint8
{
    RenderedScene UMETA(DisplayName = "Rendered Scene"),
    Webcam        UMETA(DisplayName = "Webcam"),
};

// ---------------------------------------------------------------------------
// FEstuaryCameraImage — Client -> Server `camera_image` event payload.
// Wire contract wire keys (snake_case per SDK_CONTRACT):
//   image        <- Payload.ImageJpegBase64  (base64 JPEG data)
//   mime_type    <- Payload.MimeType          (default "image/jpeg")
//   request_id   <- Payload.RequestId         (optional correlation key; matched in vision response)
//   text         <- Payload.Text              (optional prompt context)
//   sample_rate  <- Payload.SampleRate        (omit if 0)
//
// CLIENT-SIDE correlation fields NOT on the wire:
//   CharacterId, Width, Height, TimestampMs — used for response routing and diagnostics.
//
// T-07-02: ImageJpegBase64 MUST NOT be logged anywhere — security boundary.
// ---------------------------------------------------------------------------

/** Outbound camera frame payload. Populate via UEstuaryCameraComponent::DoCapture.
 *
 *  The bridge serialises only the five SDK_CONTRACT wire keys (image, mime_type,
 *  request_id, text, sample_rate). Client-side fields (CharacterId, Width, Height,
 *  TimestampMs) are used internally for routing and diagnostics only.
 */
USTRUCT(BlueprintType)
struct ESTUARY_API FEstuaryCameraImage
{
    GENERATED_BODY()

    /** Base64-encoded JPEG image data. Wire key: "image".
     *  T-07-02: NEVER log this field — even at Verbose level. */
    UPROPERTY(BlueprintReadWrite, Category = "Estuary|Camera",
              meta = (DisplayName = "Image (JPEG Base64)",
                      ToolTip = "Base64-encoded JPEG image data. Wire key: image. Never log this value."))
    FString ImageJpegBase64;

    /** MIME type of the image. Wire key: "mime_type". Default: image/jpeg. */
    UPROPERTY(BlueprintReadWrite, Category = "Estuary|Camera",
              meta = (DisplayName = "MIME Type",
                      ToolTip = "MIME type string. Wire key: mime_type. Default: image/jpeg."))
    FString MimeType = TEXT("image/jpeg");

    /** Client-generated correlation key. Wire key: "request_id" (omitted from wire if empty).
     *  Matched against bot_response.message_id to assemble FEstuaryVisionResponse. */
    UPROPERTY(BlueprintReadWrite, Category = "Estuary|Camera",
              meta = (DisplayName = "Request ID",
                      ToolTip = "Client-generated correlation key. Wire key: request_id. Matched in bot_response to route VLM result."))
    FString RequestId;

    /** Optional text prompt to send alongside the image. Wire key: "text" (omitted if empty). */
    UPROPERTY(BlueprintReadWrite, Category = "Estuary|Camera",
              meta = (DisplayName = "Text",
                      ToolTip = "Optional prompt context sent with the image. Wire key: text. Omitted from wire when empty."))
    FString Text;

    /** Audio sample rate hint. Wire key: "sample_rate" (omitted from wire when 0). Default 0. */
    UPROPERTY(BlueprintReadWrite, Category = "Estuary|Camera",
              meta = (DisplayName = "Sample Rate",
                      ToolTip = "Audio sample rate hint. Wire key: sample_rate. Omitted from wire when 0."))
    int32 SampleRate = 0;

    /** CLIENT-SIDE: owning character's ID for routing the vision response back. NOT on the wire. */
    UPROPERTY(BlueprintReadWrite, Category = "Estuary|Camera",
              meta = (DisplayName = "Character ID",
                      ToolTip = "Client-side routing field. Not transmitted. Populated by UEstuaryCameraComponent."))
    FString CharacterId;

    /** CLIENT-SIDE: capture width in pixels. Informational. NOT on the wire. */
    UPROPERTY(BlueprintReadWrite, Category = "Estuary|Camera",
              meta = (DisplayName = "Width",
                      ToolTip = "Capture width in pixels. Informational only — not transmitted."))
    int32 Width = 0;

    /** CLIENT-SIDE: capture height in pixels. Informational. NOT on the wire. */
    UPROPERTY(BlueprintReadWrite, Category = "Estuary|Camera",
              meta = (DisplayName = "Height",
                      ToolTip = "Capture height in pixels. Informational only — not transmitted."))
    int32 Height = 0;

    /** CLIENT-SIDE: capture wall-clock timestamp in milliseconds. Informational. NOT on the wire. */
    UPROPERTY(BlueprintReadWrite, Category = "Estuary|Camera",
              meta = (DisplayName = "Timestamp (ms)",
                      ToolTip = "Wall-clock capture timestamp in milliseconds. Informational only — not transmitted."))
    int64 TimestampMs = 0;
};

// ---------------------------------------------------------------------------
// FEstuaryCameraCapture — Server -> Client `camera_capture` event payload.
// Wire contract (SDK_CONTRACT.md §vision_camera):
//   request_id   -> Payload.RequestId  (required)
//   text         -> Payload.Text        (optional, empty default)
//
// T-07-01: Null-check all parsed fields; use empty-string defaults for missing
// optional keys; never pass raw JSON pointers across the marshal boundary.
// ---------------------------------------------------------------------------

/** Server-initiated capture request. Received when the server detects vision intent
 *  in user speech and proactively requests an image from the client.
 *
 *  The subsystem routes this to the registered UEstuaryCameraComponent(s).
 *  The component auto-triggers SendCameraFrame() if bHasUserConsent is true.
 */
USTRUCT(BlueprintType)
struct ESTUARY_API FEstuaryCameraCapture
{
    GENERATED_BODY()

    /** Server-provided correlation key. Wire key: "request_id". Should be echoed back in
     *  FEstuaryCameraImage.RequestId so the server can match the response. */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Camera",
              meta = (DisplayName = "Request ID",
                      ToolTip = "Server-provided correlation key. Echo back as FEstuaryCameraImage.RequestId."))
    FString RequestId;

    /** Optional text from server — may carry a prompt or intent description.
     *  Wire key: "text". Empty string default when field absent (T-07-01). */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Camera",
              meta = (DisplayName = "Text",
                      ToolTip = "Optional server-supplied text context for the capture request. Empty when absent."))
    FString Text;
};

// ---------------------------------------------------------------------------
// FEstuaryVisionResponse — CLIENT-SIDE convenience wrapper assembled by the
// subsystem from the final bot_response chunk whose message_id matches a
// pending camera request_id (SDK_CONTRACT: VLM results flow via bot_response).
//
// This is NOT a direct Socket.IO event — no "vision_response" event exists in
// SDK_CONTRACT. The subsystem synthesises this from PendingCameraRequests tracking.
// ---------------------------------------------------------------------------

/** Vision-language model result, assembled client-side by UEstuarySubsystem.
 *
 *  Broadcast via UEstuaryCharacterComponent::OnVisionResponse.
 *  bIsFinal mirrors the originating bot_response.bIsFinal.
 *  Description accumulates bot_response.Text for this request_id.
 */
USTRUCT(BlueprintType)
struct ESTUARY_API FEstuaryVisionResponse
{
    GENERATED_BODY()

    /** Matches the camera_image request_id that triggered this VLM response.
     *  Populated by the subsystem from PendingCameraRequests on dispatch. */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Camera",
              meta = (DisplayName = "Message ID",
                      ToolTip = "Matches the camera_image request_id that originated this VLM response."))
    FString MessageId;

    /** Character component that owns the originating camera request. Populated by subsystem. */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Camera",
              meta = (DisplayName = "Character ID",
                      ToolTip = "Client-side character ID populated by the subsystem on dispatch."))
    FString CharacterId;

    /** VLM response text for this chunk. Concatenate across chunks to build the full description.
     *  Maps to bot_response.Text for the camera_image request_id. */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Camera",
              meta = (DisplayName = "Description",
                      ToolTip = "VLM response text chunk. Concatenate across bIsFinal=false chunks for the full response."))
    FString Description;

    /** True on the final VLM response chunk (mirrors bot_response.bIsFinal). */
    UPROPERTY(BlueprintReadOnly, Category = "Estuary|Camera",
              meta = (DisplayName = "Is Final",
                      ToolTip = "True on the last VLM response chunk for this request_id. No further chunks after this."))
    bool bIsFinal = false;
};
