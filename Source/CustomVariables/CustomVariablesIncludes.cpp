/*
  ==============================================================================

    CustomVariablesIncludes.cpp
    Created: 10 Mar 2021 9:54:35am
    Author:  bkupe

  ==============================================================================
*/

#include "CustomVariablesIncludes.h"

//Safe here, after CustomVariablesIncludes.h has been fully processed : these pull in
//CustomVariablesModule.h, which needs CVGroup/CVGroupManager to already be declared.
#include "Module/ModuleIncludes.h"
#include "Common/Processor/ProcessorIncludes.h"

#include "Binding/CVValueMap.cpp"
#include "Binding/CVBinding.cpp"
#include "CVVariable.cpp"
#include "ui/CVVariableEditor.cpp"

#include "CVGroup.cpp"
#include "CVGroupManager.cpp"
#include "Preset/CVPreset.cpp"
#include "Preset/CVPresetManager.cpp"
#include "Preset/Morpher/MorphTarget.cpp"

#include "Preset/Morpher/Morpher.cpp"

#include "Preset/Morpher/ui/CVPresetMorphUI.cpp"
#include "Preset/Morpher/ui/MorphTargetUI.cpp"
#include "Preset/Morpher/ui/MorpherViewUI.cpp"
#include "Preset/ui/CVPresetEditor.cpp"
#include "ui/CVGroupUI.cpp"
#include "ui/CVGroupManagerUI.cpp"

