
/*
  ==============================================================================

    MappingLayer.cpp
    Created: 17 Nov 2016 8:00:02pm
    Author:  Ben Kuper

  ==============================================================================
*/

#include "TimeMachine/TimeMachineIncludes.h"
#include "Common/Processor/ProcessorIncludes.h"

MappingLayer::MappingLayer(const String &name, Sequence *_sequence, var params) :
	SequenceLayer(_sequence, name),
	alwaysUpdate(nullptr),
	sendOnSeek(nullptr),
    mappingInputSource(nullptr),
    mappingInput(nullptr)
{
	canInspectChildContainers = true;
	saveAndLoadRecursiveData = true;
	
	mapping.reset(new Mapping(var(), nullptr, false));
	mapping->editorIsCollapsed = false;
	mapping->editorCanBeCollapsed = false;
	mapping->hideEditorHeader = true;
	mapping->setHasCustomColor(false);

	alwaysUpdate = addBoolParameter("Always Update", "If checked, the mapping will be processed and output will be sent at each time change of the sequence", false);
	sendOnPlay = addBoolParameter("Send On Play", " If checked, this will force the value to go through the mapping when sequence starts playing", true);
	sendOnStop = addBoolParameter("Send On Stop", " If checked, this will force the value to go through the mapping when sequence stops playing", true);
	sendOnSeek = addBoolParameter("Send On Seek", " If checked, this will force the value to go through the mapping when jumping time", false);

	addChildControllableContainer(mapping.get());
	
	itemColor->setDefaultValue(BG_COLOR.brighter(.1f));
	
}

MappingLayer::~MappingLayer()
{
}

void MappingLayer::clearItem()
{
	//Leave the play thread before any member is destroyed : SequenceLayer only unregisters in its destructor, which runs
	//after the mapping and the automation are gone, and removeSequenceListener waits for a tick in progress.
	if (!sequence->isClearing) sequence->removeSequenceListener(this);
	SequenceLayer::clearItem();
}

void MappingLayer::setupMappingInputParameter(Parameter* source)
{
	jassert(mappingInput == nullptr);
	mappingInputSource = source;
	mappingInputSource->addParameterListener(this);

	mappingInput = ControllableFactory::createParameterFrom(source, true, true);
	mappingInput->setControllableFeedbackOnly(true);
	addParameter(mappingInput);

	mapping->lockInputTo(mappingInput);
	updateMappingInputValue();

	mappingInputReady = true;
}

void MappingLayer::updateMappingInputValue(bool forceOutput)
{
	if (!enabled->boolValue() || !sequence->enabled->boolValue()) return;

	if (evaluatesOnPlayThread())
	{
		//the play thread sets the input and processes the mapping itself (sequencePlayThreadTick)
		if (forceOutput) forceProcessOnNextTick = true;
		return;
	}

	updateMappingInputValueInternal();
	if (forceOutput || alwaysUpdate->boolValue()) mapping->process(true);
}

bool MappingLayer::evaluatesOnPlayThread()
{
	//Same predicate on both threads, from live state only, so the two paths can never both send a frame or both skip it
	return mappingInputReady && sequence->isPlaying->boolValue() && sequence->isThreadRunning()
		&& enabled->boolValue() && sequence->enabled->boolValue()
		&& !mapping->isThreadRunning() //a mapping with continuous filters is driven by its own timer thread
		&& canEvaluateOnPlayThread();
}

void MappingLayer::sequencePlayThreadTick(Sequence*, float time)
{
	if (!evaluatesOnPlayThread()) return;

	var prevValue = mappingInput->getValue();
	mappingInput->setValue(getValueAtPosition(time), true); //silent : no listener hop, the mapping is processed right here
	var newValue = mappingInput->getValue();
	bool changed = !mappingInput->checkValueIsTheSame(newValue, prevValue);

	//the silent set skipped the UI event : keep the inspector's value display alive (queued, coalesced on the message thread)
	if (changed) mappingInput->queuedNotifier.addMessage(new Parameter::ParameterEvent(Parameter::ParameterEvent::VALUE_CHANGED, mappingInput, newValue));

	if (!changed && !alwaysUpdate->boolValue() && !forceProcessOnNextTick.exchange(false)) return;

	//never wait for an edit in progress on the message thread : the next frame processes whatever it finds
	if (!mapping->processIfFree(true)) forceProcessOnNextTick = true;
}

void MappingLayer::updateMappingInputValueInternal()
{
	mappingInput->setValue(mappingInputSource->value);
}

void MappingLayer::exportBakedValues(bool dataOnly)
{
	double t = 0;
	double step = 1.0 / sequence->fps->floatValue();;
	
	Array<Array<float>> values;
	
	while (t <= sequence->totalTime->floatValue())
	{
		Array<float> pValues;
		var v = getValueAtPosition(t);
		if (v.isArray())
		{
			for (int i = 0; i < v.size(); ++i) pValues.add(v[i]);
		}
		else pValues.add((float)v);

		t += step;

		values.add(pValues);
	}

	String s;
	for (int iv = 0; iv < values.size(); iv++)
	{
		if(s.isNotEmpty()) s += "\n";

		if(!dataOnly) s += String(iv) + "\t" + String(iv * step) + "\t";

		Array<float> va = values[iv];
		for (int i = 0; i < va.size(); ++i)
		{
			s += (i > 0 ? "," : "") + String(va[i]);
		}
	}

	SystemClipboard::copyTextToClipboard(s);
	NLOG(niceName, values.size() << " keys copied to clipboard");
}


void MappingLayer::onContainerParameterChangedInternal(Parameter * p)
{
	SequenceLayer::onContainerParameterChangedInternal(p);
	if (p == enabled)
	{
		mapping->setForceDisabled(!enabled->boolValue());
	}else if (p == alwaysUpdate)
	{
		mapping->setProcessMode(alwaysUpdate->boolValue() ? Mapping::MANUAL : Mapping::VALUE_CHANGE);
	}
}

void MappingLayer::onContainerTriggerTriggered(Trigger * t)
{
	SequenceLayer::onContainerTriggerTriggered(t);
}

void MappingLayer::onControllableFeedbackUpdateInternal(ControllableContainer * cc, Controllable * c)
{
	SequenceLayer::onControllableFeedbackUpdateInternal(cc, c);
	if (c == mappingInputSource) updateMappingInputValue();
}

void MappingLayer::onExternalParameterRangeChanged(Parameter* p)
{
	if (p == mappingInputSource)
	{
		if (mappingInputSource->hasRange()) mappingInput->setRange(mappingInputSource->minimumValue, mappingInputSource->maximumValue);
		else mappingInput->clearRange();
	}
}

void MappingLayer::sequenceCurrentTimeChanged(Sequence * s, float prevTime, bool evaluateSkippedData)
{
	if (!enabled->boolValue() || !sequence->enabled->boolValue() || alwaysUpdate == nullptr || sendOnSeek == nullptr) return;

	sequenceCurrentTimeChangedInternal(s, prevTime, evaluateSkippedData);

	if (alwaysUpdate->boolValue() || (sequence->isSeeking && sendOnSeek->boolValue()))
	{
		updateMappingInputValue(true);
	}
}

void MappingLayer::sequencePlayStateChanged(Sequence * s)
{
	if (!enabled->boolValue() || !sequence->enabled->boolValue()) return;

	sequencePlayStateChangedInternal(s);

	//When playback stops, the message-thread evaluation may lag the play thread by a frame or two (its time-changed
	//notifications are still queued) : bring it up to the playhead first, otherwise the stop send below would carry the
	//older value and step backwards before the queued notification catches up.
	if (!sequence->isPlaying->boolValue()) sequenceCurrentTimeChanged(s, sequence->currentTime->floatValue(), false);

	bool updateAndProcess = (sequence->isPlaying->boolValue() && sendOnPlay->boolValue()) || (!sequence->isPlaying->boolValue() && sendOnStop->boolValue());
	if (updateAndProcess) updateMappingInputValue(true);
}
