/*
  ==============================================================================

	AutomationMappingLayer.cpp
	Created: 25 Mar 2020 1:00:31pm
	Author:  bkupe

  ==============================================================================
*/


AutomationMappingLayer::AutomationMappingLayer(const String& name, Sequence* s, var params) :
	MappingLayer(name, s, params),
	automation(nullptr)
{
	recordSendMode = addEnumParameter("Record Send Mode", "Choose what to do when recording");
	recordSendMode->addOption("Do not send", DONOTSEND)->addOption("Send original value", SEND_ORIGINAL)->addOption("Send new value", SEND_NEW);

	recorder.input->customGetTargetFunc = &ModuleManager::showAllValuesAndGetControllable;
	recorder.input->customGetControllableLabelFunc = &Module::getTargetLabelForValueControllable;
	recorder.input->customCheckAssignOnNextChangeFunc = &ModuleManager::checkControllableIsAValue;
	recorder.editorIsCollapsed = true;
	addChildControllableContainer(&recorder, false, 0);

	addKeyFromInputTrigger = addTrigger("Add Key From Input", "Grab the current value of the recorder's input and write it as a key at the current playhead position. Can be triggered from OSC, either at this trigger's own address or at /sequences/current/layers/<layer number>/addKeyFromInput to target the sequence currently opened in the editor.");

	uiHeight->setValue(120);
}

AutomationMappingLayer::~AutomationMappingLayer()
{
}

void AutomationMappingLayer::setupAutomation(Automation* a)
{
	jassert(automation == nullptr);
	automation = a;
	//automation->hideInEditor = true;
	automation->setLength(sequence->totalTime->floatValue(), true);
	automation->length->setControllableFeedbackOnly(true); //force not saving and not changing from user
	automation->length->isSavable = false;
	if (ChataigneSequenceManager::getInstance()->snapKeysToFrames->boolValue()) automation->setUnitSteps(sequence->fps->intValue());
}

var AutomationMappingLayer::getRecorderInputValue(bool* success)
{
	if (success != nullptr) *success = false;

	Parameter* inputP = dynamic_cast<Parameter*>(recorder.input->target.get());
	if (inputP == nullptr)
	{
		NLOG(niceName, "Can't add key from input : no recorder input is set.");
		return var();
	}

	var val = (recorder.normalize->enabled && recorder.normalize->boolValue()) ? inputP->getNormalizedValue() : inputP->getValue();

	if (success != nullptr) *success = true;
	return val;
}

AutomationKey* AutomationMappingLayer::getKeyToOverwriteAt(Automation* a, float time)
{
	//Overwrite window : the "Capture Overwrite Distance" app setting, floored at half a frame so exact re-captures never duplicate
	float eps = jmax(0.5f / jmax(1.0f, sequence->fps->floatValue()), ChataigneSequenceManager::getInstance()->captureOverwriteDistance->floatValue());

	//getKeyForPosition only looks backwards, so also check the next key : landing slightly before an existing key must still find it
	AutomationKey* candidates[2]{ a->getKeyForPosition(time, true), a->getNextKeyForPosition(time, true) };

	AutomationKey* result = nullptr;
	float bestDist = eps;
	for (auto& k : candidates)
	{
		if (k == nullptr) continue;
		float dist = fabsf(k->position->floatValue() - time);
		if (dist <= bestDist)
		{
			result = k;
			bestDist = dist;
		}
	}

	return result;
}

void AutomationMappingLayer::onContainerTriggerTriggered(Trigger* t)
{
	MappingLayer::onContainerTriggerTriggered(t);

	if (t == addKeyFromInputTrigger)
	{
		//OSC Remote Control fires triggers synchronously from its network thread ; key creation touches the automation, the undo history and the UI, so it must happen on the message thread
		if (MessageManager::getInstance()->isThisTheMessageThread())
		{
			addKeyAtCurrentTimeFromInput();
		}
		else
		{
			WeakReference<Inspectable> weakThis(this);
			MessageManager::callAsync([weakThis]()
				{
					if (AutomationMappingLayer* l = dynamic_cast<AutomationMappingLayer*>(weakThis.get())) l->addKeyAtCurrentTimeFromInput();
				});
		}
	}
}

void AutomationMappingLayer::updateMappingInputValueInternal()
{
	if (!recorder.isRecording->boolValue())
	{
		MappingLayer::updateMappingInputValueInternal();
	}
	else
	{
		RecordSendMode m = recordSendMode->getValueDataAsEnum<RecordSendMode>();
		if (m == SEND_ORIGINAL)
		{
			MappingLayer::updateMappingInputValueInternal();
		}
		else if (m == SEND_NEW)
		{
			if (recorder.keys.size() > 0) mappingInput->setValue(recorder.keys[recorder.keys.size() - 1].value);
		}
	}

}

void AutomationMappingLayer::selectAll(bool addToSelection)
{
	deselectThis(automation->items.size() == 0);
	automation->askForSelectAllItems(addToSelection);
}

Array<Inspectable*> AutomationMappingLayer::selectAllItemsBetweenInternal(float start, float end)
{
	Array<Inspectable*> result;
	Array<AutomationKey*> keys = automation->getKeysBetweenPositions(start, end);
	result.addArray(keys);
	return result;
}

Array<UndoableAction*> AutomationMappingLayer::getRemoveAllItemsBetweenInternal(float start, float end)
{
	return automation->getRemoveItemsUndoableAction(automation->getKeysBetweenPositions(start, end));
}

Array<UndoableAction*> AutomationMappingLayer::getInsertTimespanInternal(float start, float length)
{
	return automation->getMoveKeysBy(start, length);
}

Array<UndoableAction*> AutomationMappingLayer::getRemoveTimespanInternal(float start, float end)
{
	return automation->getRemoveTimespan(start, end);
}

void AutomationMappingLayer::getSnapTimes(Array<float>* arrayToFill)
{
	if (automation == nullptr) return;
	for (auto& i : automation->items)
	{
		arrayToFill->addIfNotAlreadyThere(i->position->floatValue());
	}
}

void AutomationMappingLayer::getSequenceSnapTimesForAutomation(Array<float>* arrayToFill, AutomationKey* k)
{
	int index = automation->items.indexOf(k);

	AutomationKey* k1 = index > 0 ? automation->items[index - 1] : nullptr;
	AutomationKey* k2 = index < automation->items.size() - 1 ? automation->items[index + 1] : nullptr;

	float t1 = k1 != nullptr ? k1->position->floatValue() : 0;
	float t2 = k2 != nullptr ? k2->position->floatValue() : sequence->totalTime->floatValue();

	sequence->getSnapTimes(arrayToFill, t1, t2);
}


void AutomationMappingLayer::sequenceCurrentTimeChangedInternal(Sequence* s, float prevTime, bool seeking)
{
	if (automation == nullptr) return;

	float curTime = sequence->currentTime->floatValue();
	automation->position->setValue(curTime);

	if (sequence->isPlaying->boolValue())
	{
		if (recorder.isRecording->boolValue())
		{
			if (sequence->currentTime->floatValue() < prevTime)  recorder.removeKeysAfter(curTime);
			recorder.addKeyAt(curTime);
		}
	}
}

void AutomationMappingLayer::sequenceTotalTimeChanged(Sequence* s)
{
	automation->setLength(s->totalTime->floatValue());
}

void AutomationMappingLayer::sequencePlayStateChangedInternal(Sequence* s)
{
	if (sequence->isPlaying->boolValue())
	{
		if (recorder.shouldRecord()) recorder.startRecording();
	}
	else if (recorder.isRecording->boolValue())
	{
		stopRecorderAndAddKeys();
	}
}

void AutomationMappingLayer::sequenceLooped(Sequence* s)
{
	//Called from the sequence's play thread. Stopping the recorder rewrites the keys, which is message-thread work like the
	//recording itself : post it, after the time changes of the last frames before the loop point, which are recorded first.
	if (!recorder.isRecording->boolValue()) return;

	if (MessageManager::getInstance()->isThisTheMessageThread())
	{
		stopRecorderAndAddKeys();
		return;
	}

	WeakReference<Inspectable> weakThis(this);
	MessageManager::callAsync([weakThis]()
		{
			if (AutomationMappingLayer* l = dynamic_cast<AutomationMappingLayer*>(weakThis.get()))
			{
				if (l->recorder.isRecording->boolValue()) l->stopRecorderAndAddKeys();
			}
		});
}

bool AutomationMappingLayer::canEvaluateOnPlayThread()
{
	//Recording is an editing activity and stays on the message thread. An armed recorder starts when the sequence plays,
	//possibly after the play thread's first tick, so it is excluded from the arm itself.
	return !recorder.isRecording->boolValue() && !recorder.arm->boolValue();
}

bool AutomationMappingLayer::paste()
{
	var data = JSON::fromString(SystemClipboard::getTextFromClipboard());
	String type = data.getProperty("itemType", "");
	if (type == automation->itemDataType)
	{
		automation->askForPaste();
		return true;
	}

	return SequenceLayer::paste();
}


SequenceLayerPanel* AutomationMappingLayer::getPanel()
{
	return new AutomationMappingLayerPanel(this);
}

SequenceLayerTimeline* AutomationMappingLayer::getTimelineUI()
{
	return new AutomationMappingLayerTimeline(this);
}

