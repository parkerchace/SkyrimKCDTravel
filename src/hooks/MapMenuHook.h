#pragma once

namespace MapMenuHook {
    // Hooks MapMenu::AdvanceMovie (vfunc 5) so an active journey can slide the
    // player arrow across the map each frame. Safe to call once after data load.
    bool Install();
}
