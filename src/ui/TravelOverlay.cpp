#include "PCH.h"
#include "TravelOverlay.h"
#include "config/Settings.h"

namespace TravelOverlay {

void ShowJourneyStart(float distance, float hours, bool byCart) {
    if (!Settings::GetSingleton().showOverlay) return;
    const int miles = static_cast<int>(distance / 5000.0f);  // rough flavour scale
    char buf[128];
    std::snprintf(buf, sizeof(buf),
        "%s You set out on a journey of some %d leagues (~%.0f hours).",
        byCart ? "The cart rolls off." : "You begin walking.", miles, hours);
    RE::DebugNotification(buf);
}

void ShowWaylaid(bool byCart) {
    if (!Settings::GetSingleton().showOverlay) return;
    RE::DebugNotification(byCart
        ? "The cart lurches to a halt..."
        : "Something stirs on the road ahead...");
}

void ShowArrival() {
    if (!Settings::GetSingleton().showOverlay) return;
    RE::DebugNotification("You arrive at your destination.");
}

}
