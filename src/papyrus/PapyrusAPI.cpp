#include "PCH.h"
#include "PapyrusAPI.h"
#include "config/Settings.h"
#include "encounters/EncounterManager.h"
#include "journey/JourneyController.h"

namespace PapyrusAPI {

namespace {
    constexpr std::string_view kScript = "Wayfarer"sv;

    // MCM calls this after the user changes settings so the DLL re-reads them.
    bool ReloadSettings(RE::StaticFunctionTag*) {
        Settings::GetSingleton().Load();
        return true;
    }

    // Arrive-then-waylay fallback. A quest script's OnPlayerFastTravelEnd calls
    // this when the C++ confirm hook is disabled/unavailable, so consequences
    // still happen (just after arrival rather than mid-route).
    bool TriggerWaylayFallback(RE::StaticFunctionTag*) {
        auto& cfg = Settings::GetSingleton();
        if (!cfg.enabled) return false;
        // Honour encounter chance even on the fallback path.
        float chance = cfg.baseEncounterChance;
        if (JourneyController::Roll100() >= chance) return false;
        // No journey heading on the fallback path — zero forward makes the spawn
        // fall back to the player's facing.
        EncounterManager::ResolveEncounter(/*byCart=*/false, RE::NiPoint3{ 0.0f, 0.0f, 0.0f }, []() {});
        return true;
    }

    // Carriage glue: a carriage travel script calls this with the destination
    // map marker to run the journey through the same controller with cart speed.
    bool StartCartJourney(RE::StaticFunctionTag*, RE::TESObjectREFR* a_destMarker) {
        if (!a_destMarker) return false;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return false;

        JourneyController::Request req;
        req.origin      = player->GetPosition();
        req.destination = a_destMarker->CreateRefHandle();
        req.byCart      = true;
        return JourneyController::Begin(req);
    }

    // True while a journey is resolving — lets Papyrus avoid double-driving.
    bool IsTraveling(RE::StaticFunctionTag*) {
        return JourneyController::IsTraveling();
    }
}

bool Register(RE::BSScript::IVirtualMachine* a_vm) {
    if (!a_vm) return false;
    a_vm->RegisterFunction("ReloadSettings", kScript, ReloadSettings);
    a_vm->RegisterFunction("TriggerWaylayFallback", kScript, TriggerWaylayFallback);
    a_vm->RegisterFunction("StartCartJourney", kScript, StartCartJourney);
    a_vm->RegisterFunction("IsTraveling", kScript, IsTraveling);
    logger::info("PapyrusAPI: registered native functions");
    return true;
}

}
