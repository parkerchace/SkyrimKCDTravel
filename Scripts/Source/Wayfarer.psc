Scriptname Wayfarer Hidden

; Native functions implemented in Wayfarer.dll (src/papyrus/PapyrusAPI.cpp).

; Re-read the INI/MCM settings. Call from the MCM after a change.
Function ReloadSettings() global native

; Arrive-then-waylay fallback. Call from the player alias' OnPlayerFastTravelEnd
; when the C++ confirm hook is disabled/unavailable. Returns True if an
; encounter was staged.
Bool Function TriggerWaylayFallback() global native

; Begin a cart/carriage journey to the given destination map marker, routed
; through the same journey controller at cart speed. Returns True if started.
Bool Function StartCartJourney(ObjectReference akDestMarker) global native

; True while a journey (including an unresolved encounter) is in progress.
Bool Function IsTraveling() global native
