/*
  ==============================================================================

    TimeTrigger.cpp
    Created: 20 Nov 2016 3:18:20pm
    Author:  Ben Kuper

  ==============================================================================
*/

#include "Common/Processor/ProcessorIncludes.h"

ChataigneTimeTrigger::ChataigneTimeTrigger(StringRef name) :
	TimeTrigger(name)
{
	csm.reset(new ConsequenceManager());
	addChildControllableContainer(csm.get());

	endCsm.reset(new ConsequenceManager("End Consequences"));
	addChildControllableContainer(endCsm.get());
}

ChataigneTimeTrigger::~ChataigneTimeTrigger()
{
}

void ChataigneTimeTrigger::onContainerParameterChangedInternal(Parameter* p)
{
	TimeTrigger::onContainerParameterChangedInternal(p);

	if (p == enabled)
	{
		csm->setForceDisabled(!enabled->boolValue());
		endCsm->setForceDisabled(!enabled->boolValue());
	}
}

void ChataigneTimeTrigger::triggerInternal()
{
	csm->triggerAll();
}

void ChataigneTimeTrigger::triggerEndInternal()
{
	endCsm->triggerAll();
}

var ChataigneTimeTrigger::getJSONData(bool includeNonOverriden)
{
	var data = TimeTrigger::getJSONData(includeNonOverriden);
	data.getDynamicObject()->setProperty("consequences", csm->getJSONData());
	data.getDynamicObject()->setProperty("endConsequences", endCsm->getJSONData());
	return data;
}

void ChataigneTimeTrigger::loadJSONDataInternal(var data)
{
	TimeTrigger::loadJSONDataInternal(data);
	csm->loadJSONData(data.getProperty("consequences", var()));
	endCsm->loadJSONData(data.getProperty("endConsequences", var())); //missing in files written before the end list existed
}
