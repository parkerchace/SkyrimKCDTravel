#include "PCH.h"
#include "RouteMath.h"

namespace RouteMath {

float Distance2D(const RE::NiPoint3& a, const RE::NiPoint3& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

RE::NiPoint3 Lerp(const RE::NiPoint3& a, const RE::NiPoint3& b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return RE::NiPoint3{
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t,
    };
}

float JourneyHours(float distance, float hoursPer100k) {
    return (distance / 100000.0f) * hoursPer100k;
}

}
