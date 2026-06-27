#pragma once
// EstuaryEditorModule.h — Editor module interface.
// Phase 9: dependency-notifier panel and details customizations added here.

#include "Modules/ModuleInterface.h"

class ESTUARYEDITOR_API FEstuaryEditorModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
};
