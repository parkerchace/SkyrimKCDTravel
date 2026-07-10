#include "PCH.h"
#include "FastTravelHook.h"
#include "config/Settings.h"
#include "journey/JourneyController.h"

namespace FastTravelHook {

namespace {
    // IMessageBoxCallback vtable layout: 0 = dtor, 1 = Run.
    constexpr std::size_t kRunVtblIdx = 1;

    using RunFn = void (*)(RE::FastTravelConfirmCallback*, RE::IMessageBoxCallback::Message);
    REL::Relocation<RunFn> s_origRun;

    void HookedRun(RE::FastTravelConfirmCallback* a_this, RE::IMessageBoxCallback::Message a_msg) {
        auto& cfg = Settings::GetSingleton();

        // DIAGNOSTIC: log every invocation so we can see whether this path fires
        // at all, and what message value the "Yes" button carries.
        RE::FormID destID = 0;
        bool mapMenuOk = (a_this && a_this->mapMenu);
        if (mapMenuOk) {
            if (auto d = a_this->mapMenu->GetRuntimeData().mapMarker.get()) {
                destID = d->GetFormID();
            }
        }
        logger::info("FastTravelHook::Run CALLED msg={} enabled={} forceFallback={} traveling={} mapMenu={} destID={:08X}",
            static_cast<int>(a_msg), cfg.enabled, cfg.forceFallback,
            JourneyController::IsTraveling(), mapMenuOk, destID);

        // The fast-travel confirm dialog delivers the "Yes/travel" result as
        // message value 1 (kUnk1) — confirmed empirically from the in-game log.
        // Only that branch performs travel; anything else (cancel) passes through.
        const bool isConfirm = (a_msg == RE::IMessageBoxCallback::Message::kUnk1);

        if (!isConfirm || !cfg.enabled || cfg.forceFallback || JourneyController::IsTraveling()) {
            logger::info("FastTravelHook::Run pass-through (isConfirm={})", isConfirm);
            return s_origRun(a_this, a_msg);
        }

        // Capture destination map marker + player origin BEFORE the native
        // teleport would fire.
        RE::ObjectRefHandle destHandle;
        if (a_this && a_this->mapMenu) {
            destHandle = a_this->mapMenu->GetRuntimeData().mapMarker;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();

        if (!player || !destHandle) {
            logger::warn("FastTravelHook::Run: no player/destination (player={} dest={}) — vanilla",
                player != nullptr, static_cast<bool>(destHandle));
            return s_origRun(a_this, a_msg);  // can't build a journey — vanilla
        }

        JourneyController::Request req;
        req.origin      = player->GetPosition();
        req.destination = destHandle;
        req.byCart      = false;

        // Start the map-animated journey. The map stays OPEN so our marker can
        // slide origin->destination; the animation closes the map and teleports
        // once it arrives.
        if (!JourneyController::BeginMapJourney(req)) {
            return s_origRun(a_this, a_msg);  // journey refused — vanilla
        }

        // Rather than fully suppress Run (which leaves the map in a half-left
        // "traveling" state that hides the location-marker layer), run the game's
        // CANCEL path (kUnk0) — a clean "return to the interactive map" that
        // performs no teleport and keeps every marker visible. Our journey
        // animation then plays over the restored map.
        logger::info("FastTravelHook: confirm intercepted — running cancel path to keep markers, journey started");
        return s_origRun(a_this, RE::IMessageBoxCallback::Message::kUnk0);
    }
}

bool Install() {
    REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE___FastTravelConfirmCallback[0] };
    s_origRun = vtbl.write_vfunc(kRunVtblIdx, HookedRun);
    logger::info("FastTravelHook: installed on FastTravelConfirmCallback::Run");
    return true;
}

}
