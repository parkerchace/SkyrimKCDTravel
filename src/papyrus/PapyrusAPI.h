#pragma once

namespace PapyrusAPI {
    // Registers Wayfarer's native functions with the Papyrus VM. Pass to
    // SKSE::GetPapyrusInterface()->Register(...).
    bool Register(RE::BSScript::IVirtualMachine* a_vm);
}
