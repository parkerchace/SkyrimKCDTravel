#include "PCH.h"
#include "config/Settings.h"
#include "encounters/EncounterManager.h"
#include "hooks/FastTravelHook.h"
#include "hooks/MapMenuHook.h"
#include "journey/JourneyController.h"
#include "papyrus/PapyrusAPI.h"

namespace {

void OnDataLoaded() {
    // Install the fast-travel confirm hook once game data is up. If it fails,
    // the Papyrus arrive-then-waylay fallback (driven by the ESP quest) still
    // provides consequences, so we never hard-break travel.
    if (!FastTravelHook::Install()) {
        logger::warn("FastTravelHook install failed — relying on Papyrus fallback");
    }
    // Drives the sliding player arrow during a journey.
    if (!MapMenuHook::Install()) {
        logger::warn("MapMenuHook install failed — arrow will not animate");
    }
    // Lets the player batter down static road debris.
    EncounterManager::InstallHitSink();
}

// Wayfarer keeps process-lifetime bookkeeping (tracked decorative props, the
// obstacle cache, debris-cluster state, the active-journey handle) that only
// makes sense for whatever save was loaded when it was created. Loading a
// DIFFERENT save (or starting a new game) without resetting these leaves
// dangling handles from the old world sitting around — exactly the kind of
// stale-reference bug that causes crashes right at/after a save load.
void OnSaveLoadBoundary() {
    EncounterManager::ResetForSaveLoad();
    JourneyController::ResetForSaveLoad();
}

void MessageListener(SKSE::MessagingInterface::Message* a_msg) {
    switch (a_msg->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            OnDataLoaded();
            break;
        case SKSE::MessagingInterface::kPreLoadGame:
        case SKSE::MessagingInterface::kNewGame:
            OnSaveLoadBoundary();
            break;
        default:
            break;
    }
}

void SetupLog() {
    auto logsPath = SKSE::log::log_directory();
    if (!logsPath) {
        SKSE::stl::report_and_fail("Wayfarer: could not find SKSE log directory");
    }
    auto logPath  = *logsPath / "Wayfarer.log";
    auto fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPath.string(), true);
    auto log      = std::make_shared<spdlog::logger>("global", fileSink);
    log->set_level(spdlog::level::info);
    log->flush_on(spdlog::level::info);
    spdlog::set_default_logger(log);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
}

} // anonymous namespace

SKSEPluginLoad(const SKSE::LoadInterface* a_skse) {
    SetupLog();
    logger::info("Wayfarer v1.0.0 loading");

    SKSE::Init(a_skse);
    Settings::GetSingleton().Load();

    if (auto* papyrus = SKSE::GetPapyrusInterface()) {
        papyrus->Register(PapyrusAPI::Register);
    } else {
        logger::error("Could not get PapyrusInterface");
    }

    auto* msg = SKSE::GetMessagingInterface();
    if (!msg) {
        logger::error("Could not get MessagingInterface");
        return false;
    }
    msg->RegisterListener(MessageListener);

    logger::info("Wayfarer loaded successfully");
    return true;
}
