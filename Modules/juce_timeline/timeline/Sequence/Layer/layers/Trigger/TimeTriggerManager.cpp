/*
  ==============================================================================

	TimeTriggerManager.cpp
	Created: 10 Dec 2016 12:22:48pm
	Author:  Ben

  ==============================================================================
*/

#include "JuceHeader.h"
#include <map>

TimeTriggerManager::TimeTriggerManager(TriggerLayer* _layer, Sequence* _sequence) :
	BaseManager("Triggers"),
	layer(_layer),
	sequence(_sequence),
	wasPlaying(false)
{
	hideInEditor = true;

	comparator.compareFunc = &TimeTriggerManager::compareTime;

	itemDataType = "TimeTrigger";

	sequence->addSequenceListener(this);

}

TimeTriggerManager::~TimeTriggerManager()
{
	if (!sequence->isClearing)
		sequence->removeSequenceListener(this);
}


void TimeTriggerManager::addTriggerAt(float time, float flagY)
{
	TimeTrigger* t = createItem();
	t->time->setValue(time);
	t->flagY->setValue(flagY);
	BaseManager::addItem(t);
}

void TimeTriggerManager::addItemInternal(TimeTrigger* t, var data)
{
	t->time->setRange(0, sequence->totalTime->floatValue());
}

void TimeTriggerManager::addItemsInternal(Array<TimeTrigger*> items, var data)
{
	for (auto& t : items) t->time->setRange(0, sequence->totalTime->floatValue());
}

Array<TimeTrigger*> TimeTriggerManager::addItemsFromClipboard(bool showWarning)
{
	Array<TimeTrigger*> triggers = BaseManager::addItemsFromClipboard(showWarning);
	if (triggers.isEmpty()) return triggers;
	if (triggers[0] == nullptr) return Array<TimeTrigger*>();

	float minTime = triggers[0]->time->floatValue();
	for (auto& tt : triggers)
	{
		if (tt->time->floatValue() < minTime)
		{
			minTime = tt->time->floatValue();
		}
	}

	float diffTime = sequence->currentTime->floatValue() - minTime;
	for (auto& tt : triggers) tt->time->setValue(tt->time->floatValue() + diffTime);

	reorderItems();

	return triggers;
}

bool TimeTriggerManager::canAddItemOfType(const String& typeToCheck)
{
	return typeToCheck == itemDataType || typeToCheck == "Action";
}

TimeTrigger* TimeTriggerManager::getPrevTrigger(float time, bool includeCurrentTime)
{
	for (int i = items.size() - 1; i >= 0; i--)
	{
		TimeTrigger* tt = items[i];
		if (tt->time->floatValue() < time || (tt->time->floatValue() == time && includeCurrentTime)) return tt;
	}
	return nullptr;
}

TimeTrigger* TimeTriggerManager::getNextTrigger(float time, bool includeCurrentTime)
{
	for (auto& tt : items)
	{
		if (tt->time->floatValue() > time || (tt->time->floatValue() == time && includeCurrentTime)) return tt;
	}
	return nullptr;
}

Array<TimeTrigger*> TimeTriggerManager::getTriggersInTimespan(float startTime, float endTime, bool includeAlreadyTriggered)
{
	Array<TimeTrigger*> result;
	for (auto& tt : items)
	{
		if (tt->time->floatValue() >= startTime && tt->time->floatValue() <= endTime && (includeAlreadyTriggered || !tt->isTriggered->boolValue()))
		{
			result.add(tt);
		}
	}
	return result;
}

Array<UndoableAction*> TimeTriggerManager::getMoveKeysBy(float start, float offset)
{
	Array<UndoableAction*> actions;
	Array<TimeTrigger*> triggers = getTriggersInTimespan(start, sequence->totalTime->floatValue());
	for (auto& t : triggers) actions.add(t->time->setUndoableValue(t->time->floatValue(), t->time->floatValue() + offset, true));
	return actions;
}

Array<UndoableAction*> TimeTriggerManager::getRemoveTimespan(float start, float end)
{
	Array<UndoableAction*> actions;
	Array<TimeTrigger*> triggers = getTriggersInTimespan(start, end);
	actions.addArray(getRemoveItemsUndoableAction(triggers));
	actions.addArray(getMoveKeysBy(end, start - end));
	return actions;
}

void TimeTriggerManager::onControllableFeedbackUpdate(ControllableContainer* cc, Controllable* c)
{
	TimeTrigger* t = static_cast<TimeTrigger*>(cc);
	if (t != nullptr)
	{
		if (c == t->time)
		{
			int index = items.indexOf(t);
			if (index > 0 && t->time->floatValue() < items[index - 1]->time->floatValue())
			{
				items.swap(index, index - 1);
				baseManagerListeners.call(&ManagerListener::itemsReordered);
			}
			else if (index < items.size() - 1 && t->time->floatValue() > items[index + 1]->time->floatValue())
			{
				items.swap(index, index + 1);
				baseManagerListeners.call(&ManagerListener::itemsReordered);
			}
		}
		else if (c == t->canTrigger || c == t->length || c == t->enabled)
		{
			sequencePlayStateChanged(nullptr);
		}

	}
}

void TimeTriggerManager::executeTriggersTimespan(float startTime, float endTime, bool forward, bool onlyUntrigger)
{
	// Build the event order from the current items so edits, removal and paste cannot leave stale pointers.
	std::multimap<float, std::pair<TimeTrigger*, bool>> actions;
	for (auto* trigger : items)
	{
		actions.emplace(trigger->time->floatValue(), std::make_pair(trigger, true));
		if (trigger->length->floatValue() > 0.f)
			actions.emplace(trigger->time->floatValue() + trigger->length->floatValue(), std::make_pair(trigger, false));
	}

	if (forward)
	{
		for (auto it = actions.lower_bound(startTime); it != actions.end() && it->first <= endTime; ++it)
			if (!onlyUntrigger || !it->second.second)
				it->second.first->setTriggerState(it->second.second, onlyUntrigger);
	}
	else
	{
		for (auto it = actions.upper_bound(endTime); it != actions.begin();)
		{
			--it;
			if (it->first < startTime) break;
			if (!onlyUntrigger || it->second.second)
			{
				bool isPointTrigger = it->second.first->length->floatValue() <= 0.f;
				bool state = !onlyUntrigger && (!it->second.second || isPointTrigger);
				it->second.first->setTriggerState(state, onlyUntrigger);
			}
		}
	}
}

void TimeTriggerManager::sequenceCurrentTimeChanged(Sequence* /*_sequence*/, float prevTime, bool evaluateSkippedData)
{
	if (!layer->enabled->boolValue() || !sequence->enabled->boolValue()) return;

	float curTime = sequence->currentTime->floatValue();
	if (curTime == prevTime) return;

	bool playingForward = sequence->playSpeed->floatValue() >= 0;
	bool diffIsForward = curTime >= prevTime;
	bool normallyPlaying = playingForward == diffIsForward;
	float minTime = jmin(prevTime, curTime);
	float maxTime = jmax(prevTime, curTime);

	if (normallyPlaying)
	{
		if ((sequence->isPlaying->boolValue() && !sequence->isSeeking) || evaluateSkippedData || ModifierKeys::getCurrentModifiers().isCtrlDown())
		{
			executeTriggersTimespan(minTime, maxTime, playingForward);
		}
	}
	else //loop or manual, untrigger
	{

		executeTriggersTimespan(minTime, maxTime, !playingForward, true);
		sequencePlayDirectionChanged(nullptr);
	}
}

void TimeTriggerManager::releaseHeldBars()
{
	//A bar that is still held has to fire its release *before* anything clears its isTriggered flag,
	//because unTrigger() early-returns on a cleared flag and the release would be lost.
	//Plain triggers are left alone : they have no end.
	Array<TimeTrigger*> triggers;
	triggers.addArray(items); //consequences may edit the manager while we iterate
	for (auto* trigger : triggers)
		if (trigger->length->floatValue() > 0.f) trigger->unTrigger();
}

void TimeTriggerManager::sequencePlayStateChanged(Sequence*)
{
	if (!layer->enabled->boolValue() || !sequence->enabled->boolValue()) return;

	if (!sequence->isPlaying->boolValue())
	{
		//Local behaviour : a bar still held when playback stops is released now, so a note started by
		//a bar can never hang past the end of playback. Only on a real playing -> stopped transition,
		//because onControllableFeedbackUpdate() calls this back to re-evaluate canTrigger while stopped.
		//Plain triggers are left alone : they have no end. Whether a released bar re-fires on resume is
		//left to the trigger's own "Evaluate conditions" setting ("Only once" does not, "Always" does).
		if (wasPlaying) releaseHeldBars();

		wasPlaying = false;
		return;
	}

	wasPlaying = true;

	float curTime = sequence->currentTime->floatValue();
	for (auto* trigger : items)
	{
		float length = trigger->length->floatValue();
		if (length > 0.f && curTime >= trigger->time->floatValue() && curTime < trigger->time->floatValue() + length)
			trigger->updateTriggerState();
	}
}

void TimeTriggerManager::sequenceTotalTimeChanged(Sequence*)
{
	for (auto& t : items) t->time->setRange(0, sequence->totalTime->floatValue());
}

void TimeTriggerManager::sequencePlayDirectionChanged(Sequence*)
{
	releaseHeldBars(); //a bar held across a direction change would otherwise lose its release
	for (auto& i : items) i->isTriggered->setValue(false);
}

void TimeTriggerManager::sequenceLooped(Sequence*)
{
	//Sequence calls this from its play thread, holding its listener list's lock. The release fires consequences, which
	//belong to the message thread like every other trigger evaluation : run there, one Stop or Set Time consequence on
	//this sequence would otherwise deadlock against a seek. Posting keeps the order : the time changes of the frames
	//before the loop point were posted earlier and are evaluated first, the loop's own time change comes after.
	if (!MessageManager::getInstance()->isThisTheMessageThread())
	{
		WeakReference<Inspectable> weakThis(this);
		MessageManager::callAsync([weakThis]()
			{
				if (TimeTriggerManager* m = dynamic_cast<TimeTriggerManager*>(weakThis.get())) m->handleLoop();
			});
		return;
	}

	handleLoop();
}

void TimeTriggerManager::handleLoop()
{
	//Release before clearing the flags : a released bar with its flag already cleared never fires its release, and
	//the rewind branch of sequenceCurrentTimeChanged would then find nothing to do. A looping sequence left notes hanging.
	releaseHeldBars();
	for (auto& t : items) t->isTriggered->setValue(false);
}

int TimeTriggerManager::compareTime(TimeTrigger* t1, TimeTrigger* t2)
{
	if (t1->time->floatValue() < t2->time->floatValue()) return -1;
	else if (t1->time->floatValue() > t2->time->floatValue()) return 1;
	return 0;
}
