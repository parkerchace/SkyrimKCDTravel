#include "PCH.h"
#include "MapMarkerDraw.h"

namespace MapMarkerDraw {

namespace {
    constexpr const char* kClip = "_root.wayfarerMarker";
    constexpr double      kDepth = 9990.0;
    constexpr float       kPi = 3.14159265f;

    bool s_logged = false;  // one-shot calibration log per journey

    void EnsureClip(RE::GFxMovieView* mv) {
        RE::GFxValue clip;
        mv->GetVariable(&clip, kClip);
        if (clip.IsObject()) return;
        RE::GFxValue root;
        mv->GetVariable(&root, "_root");
        if (!root.IsObject()) return;
        RE::GFxValue a[2];
        a[0].SetString("wayfarerMarker");
        a[1].SetNumber(kDepth);
        RE::GFxValue res;
        root.Invoke("createEmptyMovieClip", &res, a, 2);
    }

    void Call0(RE::GFxMovieView* mv, const char* method) {
        std::string full = std::string(kClip) + "." + method;
        mv->Invoke(full.c_str(), nullptr, nullptr, 0);
    }
    void MoveTo(RE::GFxMovieView* mv, double x, double y) {
        RE::GFxValue a[2]; a[0].SetNumber(x); a[1].SetNumber(y);
        mv->Invoke("_root.wayfarerMarker.moveTo", nullptr, a, 2);
    }
    void LineTo(RE::GFxMovieView* mv, double x, double y) {
        RE::GFxValue a[2]; a[0].SetNumber(x); a[1].SetNumber(y);
        mv->Invoke("_root.wayfarerMarker.lineTo", nullptr, a, 2);
    }
    void BeginFill(RE::GFxMovieView* mv, std::uint32_t rgb, float alpha) {
        RE::GFxValue a[2]; a[0].SetNumber(static_cast<double>(rgb & 0xFFFFFF)); a[1].SetNumber(alpha * 100.0);
        mv->Invoke("_root.wayfarerMarker.beginFill", nullptr, a, 2);
    }
    void EndFill(RE::GFxMovieView* mv) {
        mv->Invoke("_root.wayfarerMarker.endFill", nullptr, nullptr, 0);
    }
    void LineStyle(RE::GFxMovieView* mv, double th, std::uint32_t rgb, float alpha) {
        RE::GFxValue a[3]; a[0].SetNumber(th); a[1].SetNumber(static_cast<double>(rgb & 0xFFFFFF)); a[2].SetNumber(alpha * 100.0);
        mv->Invoke("_root.wayfarerMarker.lineStyle", nullptr, a, 3);
    }
}

bool Project(RE::GFxMovieView* mv, const RE::NiPoint3& world, float& sx, float& sy) {
    if (!mv) return false;
    auto* cam = RE::Main::WorldRootCamera();
    if (!cam) return false;

    float nx = 0.0f, ny = 0.0f, nz = 0.0f;
    const bool ok = RE::NiCamera::WorldPtToScreenPt3(
        cam->GetRuntimeData().worldToCam, cam->GetRuntimeData2().port, world, nx, ny, nz, 1e-5f);
    if (!ok) return false;

    const RE::GRectF r = mv->GetVisibleFrameRect();
    const float w = r.right - r.left;
    const float h = r.bottom - r.top;

    // WorldPtToScreenPt3 yields normalized [0,1] port coords, origin bottom-left.
    sx = r.left + nx * w;
    sy = r.top + (1.0f - ny) * h;

    if (!s_logged) {
        s_logged = true;
        logger::info("MapMarkerDraw: stage=({:.0f},{:.0f})-({:.0f},{:.0f}) proj norm=({:.3f},{:.3f},{:.3f}) -> stage=({:.0f},{:.0f})",
            r.left, r.top, r.right, r.bottom, nx, ny, nz, sx, sy);
    }
    return nz > 0.0f;
}

void DrawArrow(RE::GFxMovieView* mv, float sx, float sy, float headingDeg) {
    if (!mv) return;
    EnsureClip(mv);
    Call0(mv, "clear");

    const float a  = headingDeg * kPi / 180.0f;
    const float ca = std::cos(a), sa = std::sin(a);
    auto pt = [&](float lx, float ly, double& ox, double& oy) {
        ox = sx + lx * ca - ly * sa;
        oy = sy + lx * sa + ly * ca;
    };

    const float len = 16.0f, wid = 10.0f;
    double tx, ty, l1x, l1y, l2x, l2y, bx, by;
    pt(len, 0.0f, tx, ty);        // tip (forward)
    pt(-len * 0.6f,  wid, l1x, l1y);
    pt(-len * 0.6f, -wid, l2x, l2y);
    pt(-len * 0.3f, 0.0f, bx, by); // notch

    // Dark outline + bright fill so it reads over any map tint.
    LineStyle(mv, 2.5, 0x141414, 0.95f);
    BeginFill(mv, 0xE8C87A, 1.0f);  // warm parchment/gold
    MoveTo(mv, tx, ty);
    LineTo(mv, l1x, l1y);
    LineTo(mv, bx, by);
    LineTo(mv, l2x, l2y);
    LineTo(mv, tx, ty);
    EndFill(mv);
}

void Clear(RE::GFxMovieView* mv) {
    if (!mv) return;
    Call0(mv, "clear");
    s_logged = false;  // re-arm calibration log for the next journey
}

}
