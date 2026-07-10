#include "PCH.h"
#include "Settings.h"

// Reads settings from an already-loaded INI, keeping current values as defaults
// so a layer only overrides the keys it actually defines.
static void ReadFrom(Settings& s, CSimpleIniA& ini) {
    s.enabled       = ini.GetBoolValue("Master", "bEnabled",       s.enabled);
    s.forceFallback = ini.GetBoolValue("Master", "bForceFallback", s.forceFallback);

    s.onFootSpeed  = std::clamp(static_cast<float>(ini.GetDoubleValue("Journey", "fOnFootSpeed",  s.onFootSpeed)),  1000.0f, 200000.0f);
    s.cartSpeed    = std::clamp(static_cast<float>(ini.GetDoubleValue("Journey", "fCartSpeed",    s.cartSpeed)),    1000.0f, 200000.0f);
    s.hoursPer100k = std::clamp(static_cast<float>(ini.GetDoubleValue("Journey", "fHoursPer100k", s.hoursPer100k)), 0.0f,    48.0f);
    s.checkpoints  = std::clamp(static_cast<int>(ini.GetLongValue("Journey", "iCheckpoints", s.checkpoints)), 1, 12);
    s.blockWhenOverEncumbered = ini.GetBoolValue("Journey", "bBlockWhenOverEncumbered", s.blockWhenOverEncumbered);

    s.baseEncounterChance     = std::clamp(static_cast<float>(ini.GetDoubleValue("Encounters", "fBaseChance",      s.baseEncounterChance)), 0.0f, 100.0f);
    s.nightMultiplier         = std::clamp(static_cast<float>(ini.GetDoubleValue("Encounters", "fNightMultiplier", s.nightMultiplier)),     0.0f, 10.0f);
    s.maxEncountersPerJourney = std::clamp(static_cast<int>(ini.GetLongValue("Encounters", "iMaxPerJourney", s.maxEncountersPerJourney)), 0, 10);
    s.encounterSource         = std::clamp(static_cast<int>(ini.GetLongValue("Encounters", "iSource", s.encounterSource)), 0, 2);
    s.avoidBaseChance         = std::clamp(static_cast<float>(ini.GetDoubleValue("Encounters", "fAvoidBaseChance", s.avoidBaseChance)), 0.0f, 100.0f);
    s.wCombat    = std::clamp(static_cast<float>(ini.GetDoubleValue("Encounters", "fWeightCombat",    s.wCombat)),    0.0f, 1000.0f);
    s.wTraveler  = std::clamp(static_cast<float>(ini.GetDoubleValue("Encounters", "fWeightTraveler",  s.wTraveler)),  0.0f, 1000.0f);
    s.wDiscovery = std::clamp(static_cast<float>(ini.GetDoubleValue("Encounters", "fWeightDiscovery", s.wDiscovery)), 0.0f, 1000.0f);
    s.wMoral     = std::clamp(static_cast<float>(ini.GetDoubleValue("Encounters", "fWeightMoral",     s.wMoral)),     0.0f, 1000.0f);
    s.wHazard    = std::clamp(static_cast<float>(ini.GetDoubleValue("Encounters", "fWeightHazard",    s.wHazard)),    0.0f, 1000.0f);
    s.wildernessDangerChance = std::clamp(static_cast<float>(ini.GetDoubleValue("Danger", "fChance",       s.wildernessDangerChance)), 0.0f, 100.0f);
    s.wDangerAnimals  = std::clamp(static_cast<float>(ini.GetDoubleValue("Danger", "fWeightAnimals",  s.wDangerAnimals)),  0.0f, 1000.0f);
    s.wDangerMonsters = std::clamp(static_cast<float>(ini.GetDoubleValue("Danger", "fWeightMonsters", s.wDangerMonsters)), 0.0f, 1000.0f);
    s.wDangerBandits  = std::clamp(static_cast<float>(ini.GetDoubleValue("Danger", "fWeightBandits",  s.wDangerBandits)),  0.0f, 1000.0f);
    s.wDangerRobbers  = std::clamp(static_cast<float>(ini.GetDoubleValue("Danger", "fWeightRobbers",  s.wDangerRobbers)),  0.0f, 1000.0f);
    s.wDangerDragons  = std::clamp(static_cast<float>(ini.GetDoubleValue("Danger", "fWeightDragons",  s.wDangerDragons)),  0.0f, 1000.0f);
    s.encounterActorCount     = std::clamp(static_cast<int>(ini.GetLongValue("Encounters", "iActorCount", s.encounterActorCount)), 1, 12);
    s.pairChance              = std::clamp(static_cast<float>(ini.GetDoubleValue("Encounters", "fPairChance", s.pairChance)), 0.0f, 100.0f);

    // Read FormIDs as strings so both hex (0x...) and decimal work regardless of
    // the INI reader's numeric base.
    auto readForm = [&](const char* key, unsigned int cur) -> unsigned int {
        const char* raw = ini.GetValue("Encounters", key, nullptr);
        if (!raw || !*raw) return cur;
        return static_cast<unsigned int>(std::strtoul(raw, nullptr, 0));
    };
    s.encounterActor[0] = readForm("iActorForm0", s.encounterActor[0]);
    s.encounterActor[1] = readForm("iActorForm1", s.encounterActor[1]);
    s.encounterActor[2] = readForm("iActorForm2", s.encounterActor[2]);
    s.encounterActor[3] = readForm("iActorForm3", s.encounterActor[3]);
    s.encounterActorPlugin = ini.GetValue("Encounters", "sActorPlugin", s.encounterActorPlugin.c_str());

    s.showOverlay      = ini.GetBoolValue("Display", "bShowOverlay", s.showOverlay);
    s.mapTravelSeconds = std::clamp(static_cast<float>(ini.GetDoubleValue("Display", "fMapTravelSeconds", s.mapTravelSeconds)), 1.0f, 60.0f);
    s.mapCameraFollow  = ini.GetBoolValue("Display", "bMapCameraFollow", s.mapCameraFollow);
}

void Settings::Load() {
    CSimpleIniA ini;
    ini.SetUnicode();
    if (ini.LoadFile(kIniPath) < SI_OK) {
        logger::warn("Settings: could not load {}; using defaults.",
            "Data/SKSE/Plugins/Wayfarer.ini");
    } else {
        ReadFrom(*this, ini);
    }

    // MCM Helper writes user overrides here; load second so the MCM wins.
    CSimpleIniA mcm;
    mcm.SetUnicode();
    if (mcm.LoadFile(kMcmIniPath) >= SI_OK) {
        ReadFrom(*this, mcm);
        logger::info("Settings: MCM overrides applied");
    }

    logger::info("Settings loaded: enabled={} source={} baseChance={:.0f} checkpoints={}",
        enabled, encounterSource, baseEncounterChance, checkpoints);
}

void Settings::Save() {
    std::lock_guard lock(iniMutex);
    CSimpleIniA ini;
    ini.SetUnicode();
    ini.LoadFile(kIniPath);

    ini.SetBoolValue  ("Master", "bEnabled",       enabled);
    ini.SetBoolValue  ("Master", "bForceFallback", forceFallback);
    ini.SetDoubleValue("Journey", "fOnFootSpeed",  static_cast<double>(onFootSpeed));
    ini.SetDoubleValue("Journey", "fCartSpeed",    static_cast<double>(cartSpeed));
    ini.SetDoubleValue("Journey", "fHoursPer100k", static_cast<double>(hoursPer100k));
    ini.SetLongValue  ("Journey", "iCheckpoints",  checkpoints);
    ini.SetBoolValue  ("Journey", "bBlockWhenOverEncumbered", blockWhenOverEncumbered);
    ini.SetDoubleValue("Encounters", "fBaseChance",      static_cast<double>(baseEncounterChance));
    ini.SetDoubleValue("Encounters", "fNightMultiplier", static_cast<double>(nightMultiplier));
    ini.SetLongValue  ("Encounters", "iMaxPerJourney",   maxEncountersPerJourney);
    ini.SetLongValue  ("Encounters", "iSource",          encounterSource);
    ini.SetDoubleValue("Encounters", "fAvoidBaseChance", static_cast<double>(avoidBaseChance));
    ini.SetLongValue  ("Encounters", "iActorCount",      encounterActorCount);
    ini.SetBoolValue  ("Display", "bShowOverlay", showOverlay);
    ini.SetDoubleValue("Display", "fMapTravelSeconds", static_cast<double>(mapTravelSeconds));
    ini.SetBoolValue  ("Display", "bMapCameraFollow", mapCameraFollow);

    if (ini.SaveFile(kIniPath) < SI_OK) {
        logger::error("Settings::Save - failed to write INI");
    } else {
        logger::info("Settings saved");
    }
}
