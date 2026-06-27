// EstuaryMarshal.h — THE single chokepoint for UObject access from non-game threads (Pitfall 1/3/17).
//
// PITFALL 1:  UObject methods, UFUNCTION calls, and DYNAMIC_MULTICAST_DELEGATE::Broadcast()
//             MUST run on the game thread. Socket.IO / audio / render callbacks do not.
// PITFALL 3:  Capture via TWeakObjectPtr<T>, never raw `this`. Validate on game thread before use.
// PITFALL 17: check(IsInGameThread()) is INSIDE the dispatched lambda — not at the call site —
//             so it fires precisely where the constraint matters.
//
// Usage:
//   Estuary::MarshalToGameThread(this, [](UMySubsystem* Self)
//   {
//       Self->OnFoo.Broadcast(...);   // safely on game thread
//   });
//
// The template parameter T is inferred from Object; CallableT is inferred from the lambda.
// Call site may be on any thread. The lambda always executes on ENamedThreads::GameThread.

#pragma once

#include "CoreTypes.h"
#include "Misc/AssertionMacros.h"
#include "Async/Async.h"
#include "UObject/WeakObjectPtr.h"
#include "Templates/UnrealTemplate.h"

namespace Estuary
{
    /**
     * Marshal a call onto the game thread, lifetime-guarded via TWeakObjectPtr.
     *
     * PITFALL 1 / 3 / 17: UObject touches, UFUNCTION calls, and dynamic
     * delegate Broadcast() MUST happen on the game thread. Every off-thread
     * callback (Socket.IO worker, audio capture, render thread) funnels here.
     * Capture is TWeakObjectPtr-based; the lambda validates IsValid() before
     * dereferencing so PIE-stop / actor-destroy races are safe.
     *
     * @param Object    The UObject target. Must not be nullptr at the call site
     *                  (asserted in non-Shipping builds). Lambda re-validates
     *                  via WeakThis.Get() before invoking Callable.
     * @param Callable  Lambda of signature void(T*). Runs on the game thread.
     *                  May touch UObject state, call UFUNCTIONs, or Broadcast()
     *                  DYNAMIC_MULTICAST_DELEGATEs.
     */
    template <typename T, typename CallableT>
    FORCEINLINE void MarshalToGameThread(T* Object, CallableT&& Callable)
    {
        // Early-out: capturing an already-null pointer into an AsyncTask
        // is a developer bug; fail loudly in non-Shipping, no-op in Shipping.
        if (!Object)
        {
            checkSlow(Object != nullptr);
            return;
        }

        TWeakObjectPtr<T> WeakThis(Object);
        AsyncTask(ENamedThreads::GameThread,
            [WeakThis, Callable = Forward<CallableT>(Callable)]()
            {
                // Pitfall 17 runtime assertion — every UObject touch MUST land on GT.
                check(IsInGameThread());

                if (T* Resolved = WeakThis.Get())
                {
                    Callable(Resolved);
                }
                // else: source UObject was GC'd or PIE stopped between enqueue
                //       and dispatch — swallow silently. The subsystem's
                //       Deinitialize() path already logged the teardown.
            });
    }

} // namespace Estuary
