#pragma once
// EstuaryModule.h — Core runtime module interface.
// Phases 2-9 extend this module with subsystem, character, and audio classes.

#include "Modules/ModuleInterface.h"

class ESTUARY_API FEstuaryModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
};
