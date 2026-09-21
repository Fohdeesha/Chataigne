/*
  ==============================================================================

    CustomVariablesIncludes.h
    Created: 10 Mar 2021 9:54:35am
    Author:  bkupe

  ==============================================================================
*/

#pragma once

#include "JuceHeader.h"

#include "Preset/Morpher/MorphTarget.h"

#include "Preset/CVPreset.h"
#include "Preset/CVPresetManager.h"

#include "Preset/Morpher/jc_voronoi.h"
#include "Preset/Morpher/Morpher.h"

//Bindings. CVBinding.h only forward-declares MappingOutputManager : including
//BaseCommandHandler.h from here would be circular (it pulls in ModuleIncludes.h,
//which pulls in CustomVariablesModule.h, which pulls in this file).
#include "Binding/CVValueMap.h"
#include "Binding/CVBinding.h"
#include "CVVariable.h"

#include "CVGroup.h"
#include "CVGroupManager.h"

#include "ui/CVVariableEditor.h"

#include "Preset/Morpher/ui/CVPresetMorphUI.h"
#include "Preset/Morpher/ui/MorphTargetUI.h"
#include "Preset/Morpher/ui/MorpherViewUI.h"

#include "Preset/ui/CVPresetEditor.h"
#include "ui/CVGroupUI.h"
#include "ui/CVGroupManagerUI.h"

