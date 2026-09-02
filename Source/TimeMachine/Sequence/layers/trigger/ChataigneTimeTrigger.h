/*
  ==============================================================================

    ChataigneTimeTrigger.h
    Created: 20 Nov 2016 3:18:20pm
    Author:  Ben Kuper

  ==============================================================================
*/

#pragma once

class ConsequenceManager;

class ChataigneTimeTrigger :
	public TimeTrigger
{
public:
	ChataigneTimeTrigger(StringRef name = "Trigger");
	virtual ~ChataigneTimeTrigger();

	std::unique_ptr<ConsequenceManager> csm;
	std::unique_ptr<ConsequenceManager> endCsm; //fired when the playhead leaves a trigger that has a length

	virtual void onContainerParameterChangedInternal(Parameter* p) override;

	virtual void triggerInternal() override;
	virtual void triggerEndInternal() override;

	virtual var getJSONData(bool includeNonOverriden = false) override;
	virtual void loadJSONDataInternal(var data) override;
};
