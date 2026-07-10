#pragma once

// Small pure-math helpers for a straight-line route between two world points.
// Kept isolated so the "true mid-route placement" upgrade (v1.1: snap an
// interpolated point to the nearest road/navmesh) has one obvious home.
namespace RouteMath {
    // Horizontal (XY) distance; Z is ignored so mountains don't inflate travel.
    float Distance2D(const RE::NiPoint3& a, const RE::NiPoint3& b);

    // Point a fraction t in [0,1] along origin->dest (all three axes).
    RE::NiPoint3 Lerp(const RE::NiPoint3& a, const RE::NiPoint3& b, float t);

    // In-game hours a journey of the given world-unit distance should cost.
    float JourneyHours(float distance, float hoursPer100k);
}
