#pragma once

// Draws Wayfarer's own travel marker directly into the MapMenu's GFx movie,
// projected through the world-map camera each frame so it stays glued to the
// terrain as the player pans/zooms. Crucially it NEVER touches the native player
// marker or location icons — those stay exactly as the game drew them.
namespace MapMarkerDraw {
    // Project a world point to the map movie's stage coordinates. Returns false
    // if the camera is unavailable or the point is behind it.
    bool Project(RE::GFxMovieView* a_movie, const RE::NiPoint3& a_world, float& a_sx, float& a_sy);

    // Draw the travel arrow at stage coords, pointing along headingDeg (screen
    // space, degrees). Replaces any previous frame's drawing.
    void DrawArrow(RE::GFxMovieView* a_movie, float a_sx, float a_sy, float a_headingDeg);

    // Remove our drawing (call when the journey ends / map closes).
    void Clear(RE::GFxMovieView* a_movie);
}
