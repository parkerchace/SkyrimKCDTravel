#pragma once

namespace FastTravelHook {
    // Installs the vtable hook on FastTravelConfirmCallback::Run. Returns false
    // if the hook could not be installed (caller should rely on the Papyrus
    // arrive-then-waylay fallback instead). Safe to call once, after kDataLoaded.
    bool Install();
}
