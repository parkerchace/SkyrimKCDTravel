#pragma once

// v1 presentation layer. Currently narrates the journey through HUD
// notifications. This is the seam where the Skyrim Loading Percent Scaleform
// "render-into-a-GFx-movie" technique gets ported in v1.1 to draw the overhead
// map, route line, and sliding travel marker — the function signatures here are
// deliberately what that richer overlay will also drive.
namespace TravelOverlay {
    void ShowJourneyStart(float distance, float hours, bool byCart);
    void ShowWaylaid(bool byCart);
    void ShowArrival();
}
