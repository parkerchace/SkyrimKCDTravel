Scriptname WayfarerPlayerAlias extends ReferenceAlias

; Attach this to a ReferenceAlias that points at the PlayerRef, inside the
; Wayfarer quest (start game enabled). It provides the arrive-then-waylay
; fallback path: when the C++ confirm hook is off or unavailable, vanilla fast
; travel still fires OnPlayerFastTravelEnd here and we stage consequences.

Event OnPlayerFastTravelEnd(float afTravelGameTimeHours)
    ; The DLL decides (by settings + roll) whether to actually stage an encounter.
    Wayfarer.TriggerWaylayFallback()
EndEvent
