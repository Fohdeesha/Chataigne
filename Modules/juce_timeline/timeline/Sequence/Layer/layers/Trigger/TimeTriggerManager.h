/*
  ==============================================================================

	TimeTriggerManager.h
	Created: 10 Dec 2016 12:22:48pm
	Author:  Ben

  ==============================================================================
*/

#pragma once

class TriggerLayer;


class TimeTriggerManager :
	public BaseManager<TimeTrigger>,
	public Sequence::SequenceListener
{
public:
	TimeTriggerManager(TriggerLayer* layer, Sequence* sequence);
	virtual ~TimeTriggerManager();

	TriggerLayer* layer;
	Sequence* sequence;

	//sequencePlayStateChanged() is also called back internally to re-evaluate canTrigger while
	//stopped, so the release-on-pause below needs a real playing -> stopped transition to act on
	bool wasPlaying;

	virtual void addTriggerAt(float time, float flagYPos);

	void addItemInternal(TimeTrigger* t, var data) override;
	void addItemsInternal(Array<TimeTrigger*> items, var data) override;

	Array<TimeTrigger*> addItemsFromClipboard(bool showWarning = true) override;
	bool canAddItemOfType(const String& typeToCheck) override;

	TimeTrigger* getPrevTrigger(float time, bool includeCurrentTime = false);
	TimeTrigger* getNextTrigger(float time, bool includeCurrentTime = false);
	Array<TimeTrigger*> getTriggersInTimespan(float startTime, float endTime, bool includeAlreadyTriggered = false);

	Array<UndoableAction*> getMoveKeysBy(float start, float offset);
	Array<UndoableAction*> getRemoveTimespan(float start, float end);

	void onControllableFeedbackUpdate(ControllableContainer* cc, Controllable* c) override;
	void executeTriggersTimespan(float startTime, float endTime, bool forward, bool onlyUntrigger = false);
	void releaseHeldBars();

	void sequenceCurrentTimeChanged(Sequence* _sequence, float prevTime, bool evaluateSkippedData) override;
	void sequencePlayStateChanged(Sequence*) override;
	void sequenceTotalTimeChanged(Sequence*) override;
	void sequencePlayDirectionChanged(Sequence*) override;
	void sequenceLooped(Sequence*) override;
	void handleLoop(); //message thread

	static int compareTime(TimeTrigger* t1, TimeTrigger* t2);

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TimeTriggerManager)
};
