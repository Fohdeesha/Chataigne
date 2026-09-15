/*
  ==============================================================================

	MappingLayer.h
	Created: 17 Nov 2016 8:00:02pm
	Author:  Ben Kuper

  ==============================================================================
*/

#pragma once

class GradientColorManager;
class Mapping;


class MappingLayer :
	public SequenceLayer
{
public:
	MappingLayer(const String& name, Sequence* _sequence, var params);
	~MappingLayer();

	BoolParameter* alwaysUpdate;
	BoolParameter* sendOnPlay;
	BoolParameter* sendOnStop;
	BoolParameter* sendOnSeek;

	Parameter* mappingInputSource;
	Parameter* mappingInput;
	std::unique_ptr<Mapping> mapping;

	void setupMappingInputParameter(Parameter* source);

	void updateMappingInputValue(bool forceOutput = false);
	virtual void updateMappingInputValueInternal();

	virtual var getValueAtPosition(float position) = 0;
	void exportBakedValues(bool dataOnly = false);

	virtual void onContainerParameterChangedInternal(Parameter* p) override;
	virtual void onContainerTriggerTriggered(Trigger* t) override;
	void onControllableFeedbackUpdateInternal(ControllableContainer* cc, Controllable* c) override;
	void onExternalParameterRangeChanged(Parameter* p) override;

	void sequenceCurrentTimeChanged(Sequence*, float prevTime, bool evaluateSkippedData) override;
	virtual void sequenceCurrentTimeChangedInternal(Sequence*, float prevTime, bool evaluateSkippedData) {};
	void sequencePlayStateChanged(Sequence*) override;
	virtual void sequencePlayStateChangedInternal(Sequence*) {}

	//Play-thread evaluation. While the sequence plays, its play thread evaluates this layer at the precise frame time and
	//sends the result itself (sequencePlayThreadTick), so the output does not wait for the message thread, which is also
	//the thread that paints the interface. The message-thread path (sequenceCurrentTimeChanged -> automation value ->
	//updateMappingInputValue) keeps driving the cursor and the value displays, but must not send while the play thread does,
	//or it would queue an older value behind the play thread's. getValueAtPosition is then called from the play thread :
	//implementations must hold their key lock.
	std::atomic<bool> mappingInputReady{ false }; //set once setupMappingInputParameter is done, so a tick never sees a half-built layer
	std::atomic<bool> forceProcessOnNextTick{ false }; //a forced update (send on play / seek) or a frame skipped because the mapping was busy
	bool evaluatesOnPlayThread();
	virtual bool canEvaluateOnPlayThread() { return true; }
	void sequencePlayThreadTick(Sequence* s, float time) override;

	virtual void clearItem() override;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MappingLayer)
};