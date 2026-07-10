#include "PCH.h"
#include "MapMenuHook.h"
#include "journey/JourneyController.h"

namespace MapMenuHook {

namespace {
    // IMenu vtable: AdvanceMovie is vfunc 5 (see MapMenu.h override comments).
    constexpr std::size_t kAdvanceMovieIdx = 5;

    using AdvanceMovieFn = void (*)(RE::MapMenu*, float, std::uint32_t);
    REL::Relocation<AdvanceMovieFn> s_origAdvance;

    void HookedAdvanceMovie(RE::MapMenu* a_this, float a_interval, std::uint32_t a_currentTime) {
        // Let the map render normally first, then draw OUR marker on top of it.
        // We no longer touch the native player marker or location icons, so this
        // never disturbs them.
        s_origAdvance(a_this, a_interval, a_currentTime);
        if (JourneyController::IsAnimating()) {
            JourneyController::TickMapAnimation(a_this, a_interval);
        }
    }
}

bool Install() {
    REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_MapMenu[0] };
    s_origAdvance = vtbl.write_vfunc(kAdvanceMovieIdx, HookedAdvanceMovie);
    logger::info("MapMenuHook: installed on MapMenu::AdvanceMovie");
    return true;
}

}
