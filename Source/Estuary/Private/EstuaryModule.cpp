// EstuaryModule.cpp — Core runtime module entry point.
// Phase 2+: UEstuarySubsystem and transport wiring added here.
#include "EstuaryModule.h"
#include "EstuaryLog.h"
#include "Modules/ModuleManager.h"

void FEstuaryModule::StartupModule()
{
    UE_LOG(LogEstuary, Log, TEXT("Estuary module started"));
}

void FEstuaryModule::ShutdownModule()
{
    UE_LOG(LogEstuary, Log, TEXT("Estuary module shut down"));
}

IMPLEMENT_MODULE(FEstuaryModule, Estuary)
