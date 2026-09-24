/*
  ==============================================================================

	MIDIModule.cpp
	Created: 20 Dec 2016 12:35:26pm
	Author:  Ben

  ==============================================================================
*/

#include "Module/ModuleIncludes.h"

MIDIValueComparator MIDIModule::midiValueComparator;

MIDIModule::MIDIModule(const String& name, bool _useGenericControls) :
	Module(name),
	manualAddMode(false),
	inputDevice(nullptr),
	outputDevice(nullptr),
	tempoCC("Tempo"),
	lastClockReceiveTime(0),
	lastClockReceiveTimeIndex(0),
	mtcCC("MTC"),
	infoCC("Infos"),
	useGenericControls(_useGenericControls),
	inputQueueUpdater(*this),
	noteOffTimer(*this)
{
	valuesCC.customControllableComparator = &MIDIModule::midiValueComparator;

	canHandleRouteValues = true;
	includeValuesInSave = true;

	if (useGenericControls)
	{
		autoAdd = moduleParams.addBoolParameter("Auto Add", "Auto Add MIDI values that are received but not in the list", true);
		defManager->add(CommandDefinition::createDef(this, "", "Note On", &MIDINoteAndCCCommand::create)->addParam("type", (int)MIDINoteAndCCCommand::NOTE_ON));
		defManager->add(CommandDefinition::createDef(this, "", "Note Off", &MIDINoteAndCCCommand::create)->addParam("type", (int)MIDINoteAndCCCommand::NOTE_OFF));
		defManager->add(CommandDefinition::createDef(this, "", "Full Note", &MIDINoteAndCCCommand::create)->addParam("type", (int)MIDINoteAndCCCommand::FULL_NOTE));
		defManager->add(CommandDefinition::createDef(this, "", "Control Change", &MIDINoteAndCCCommand::create)->addParam("type", (int)MIDINoteAndCCCommand::CONTROLCHANGE));
		defManager->add(CommandDefinition::createDef(this, "", "Sysex Message", &MIDISysExCommand::create));
		defManager->add(CommandDefinition::createDef(this, "", "Program Change", &MIDINoteAndCCCommand::create)->addParam("type", (int)MIDINoteAndCCCommand::PROGRAMCHANGE));
		defManager->add(CommandDefinition::createDef(this, "", "Pitch Wheel", &MIDINoteAndCCCommand::create)->addParam("type", (int)MIDINoteAndCCCommand::PITCH_WHEEL));
		defManager->add(CommandDefinition::createDef(this, "", "Channel Pressure", &MIDINoteAndCCCommand::create)->addParam("type", (int)MIDINoteAndCCCommand::CHANNEL_PRESSURE));
		defManager->add(CommandDefinition::createDef(this, "", "After Touch", &MIDINoteAndCCCommand::create)->addParam("type", (int)MIDINoteAndCCCommand::AFTER_TOUCH));
		defManager->add(CommandDefinition::createDef(this, "", "MMC Command", &MIDIMidiMachineControlCommandAndGoto::create)->addParam("goto", false));
		defManager->add(CommandDefinition::createDef(this, "", "MMC Goto", &MIDIMidiMachineControlCommandAndGoto::create)->addParam("goto", true));
	}

	useHierarchy = moduleParams.addBoolParameter("Use Hierarchy", "If checked, incoming messages will be sorted in nested containers instead of 1-level", false);
	autoFeedback = moduleParams.addBoolParameter("Auto Feedback", "If checked, all changed values will be resent automatically to the outputs", false);

	octaveShift = moduleParams.addIntParameter("Octave Shift", "This allows to adjust to other software's conventions when converting Pitch to Note name. Because MIDI sucks.", 0, -5, 5);

	usePitchForNoteNames = moduleParams.addBoolParameter("Use Pitch For Note Names", "If checked, the note names will be displayed as pitch values instead of note names", false);

	midiParam = new MIDIDeviceParameter("Devices");
	moduleParams.addParameter(midiParam);

	isConnected = moduleParams.addBoolParameter("Is Connected", "This is checked if the module is connected to at least one input or output device", false);
	isConnected->setControllableFeedbackOnly(true);
	isConnected->isSavable = false;
	connectionFeedbackRef = isConnected;


	//Script
	scriptObject.getDynamicObject()->setMethod(sendNoteOnId, &MIDIModule::sendNoteOnFromScript);
	scriptObject.getDynamicObject()->setMethod(sendNoteOffId, &MIDIModule::sendNoteOffFromScript);
	scriptObject.getDynamicObject()->setMethod(sendCCId, &MIDIModule::sendCCFromScript);
	scriptObject.getDynamicObject()->setMethod(sendSysexId, &MIDIModule::sendSysexFromScript);
	scriptObject.getDynamicObject()->setMethod(sendProgramChangeId, &MIDIModule::sendProgramChangeFromScript);
	scriptObject.getDynamicObject()->setMethod(sendPitchWheelId, &MIDIModule::sendPitchWheelFromScript);
	scriptObject.getDynamicObject()->setMethod(sendChannelPressureId, &MIDIModule::sendChannelPressureFromScript);
	scriptObject.getDynamicObject()->setMethod(sendAfterTouchId, &MIDIModule::sendAfterTouchFromScript);
	scriptObject.getDynamicObject()->setMethod(sendMachineControlCommandId, &MIDIModule::sendMidiMachineControlCommandFromScript);
	scriptObject.getDynamicObject()->setMethod(sendMachineControlGotoId, &MIDIModule::sendMidiMachineControlGotoFromScript);

	scriptManager->scriptTemplate += ChataigneAssetManager::getInstance()->getScriptTemplate("midi");

	valuesCC.userCanAddControllables = true;
	valuesCC.customUserCreateControllableFunc = &MIDIModule::showMenuAndCreateValue;
	alwaysShowValues = true;


	notePlayed = infoCC.addTrigger("Note Played", "A note has been played");
	oneNoteOn = infoCC.addBoolParameter("One Note On", "At least one note is playing right now", false);
	lastChannel = infoCC.addIntParameter("Last Channel", "Last played channel", 1, 1, 16);
	lastPitch = infoCC.addIntParameter("Last Pitch", "Last played pitch", 0, 0, 127);
	lastVelocity = infoCC.addIntParameter("Last Velocity", "Last played velocity", 0, 0, 127);
	valuesCC.addChildControllableContainer(&infoCC);

	bpm = tempoCC.addFloatParameter("BPM", "BPM detected by the incoming MIDI Clock", 0, 0, 999);
	sendClock = tempoCC.addBoolParameter("Send Clock", "If checked, send MIDI Clock to the output. If not, receiving incoming MIDI Clock", false);
	midiStartTrigger = tempoCC.addTrigger("Start", "Clock Start signal");
	midiStopTrigger = tempoCC.addTrigger("Stop", "Clock Stop signal");
	midiContinueTrigger = tempoCC.addTrigger("Continue", "Clock Continue signal");

	for (int i = 0; i < 24; i++) clockDeltaTimes[i] = 0;
	valuesCC.addChildControllableContainer(&tempoCC);

	mtcTime = mtcCC.addFloatParameter("MTC Time", "Time sent by the MTC.", 0, 0);
	mtcTime->defaultUI = FloatParameter::TIME;
	mtcIsPlaying = mtcCC.addBoolParameter("Is MTC Playing", "Is MTC currently playing ?", false);
	valuesCC.addChildControllableContainer(&mtcCC);

	thruManager.reset(new ControllableContainer("Pass-through"));
	thruManager->userCanAddControllables = true;
	thruManager->customUserCreateControllableFunc = &MIDIModule::createThruControllable;
	moduleParams.addChildControllableContainer(thruManager.get());

	setupIOConfiguration(inputDevice != nullptr, outputDevice != nullptr);
}

MIDIModule::~MIDIModule()
{
	//clearItem() normally did all of this already, while the module was whole
	noteOffTimer.stopTimer();
	inputQueueUpdater.cancelPendingUpdate();
	outClock.stop();
	if (inputDevice != nullptr) inputDevice->removeMIDIInputListener(this);
	mtcReceiver.reset();
	MIDIOutputDevice* d = nullptr;
	{
		const ScopedLock sl(outputLock);
		d = outputDevice;
		outputDevice = nullptr;
	}
	if (d != nullptr) d->close();
}

void MIDIModule::clearItem()
{
	//The driver's thread, the queue and the note off timer must all be done with this module before Module::clearItem()
	//and the destructor take apart what they report into. Owed note offs go out now, while the output is still open.
	if (inputDevice != nullptr) inputDevice->removeMIDIInputListener(this); //waits for a message being dispatched
	inputDevice = nullptr;
	mtcReceiver.reset();

	inputQueueUpdater.cancelPendingUpdate();
	{
		const ScopedLock sl(inputQueueLock);
		inputQueue.clear();
	}

	sendPendingNoteOffs(true);
	noteOffTimer.stopTimer();
	outClock.stop();

	MIDIOutputDevice* d = nullptr;
	{
		const ScopedLock sl(outputLock);
		d = outputDevice;
		outputDevice = nullptr;
	}
	outClock.setOutDevice(nullptr);
	if (d != nullptr) d->close(); //sends what is still queued first

	Module::clearItem();
}

bool MIDIModule::withOutput(std::function<void(MIDIOutputDevice*)> f)
{
	const ScopedLock sl(outputLock);
	if (outputDevice == nullptr) return false;
	f(outputDevice);
	return true;
}

//Each send : the checks, the log and the activity trigger first, the device only under outputLock (nothing that can wait
//on another lock runs while holding it). The unlocked outputDevice test is only a shortcut, withOutput() decides.

void MIDIModule::sendNoteOn(int channel, int pitch, int velocity)
{
	if (!enabled->boolValue()) return;
	if (outputDevice == nullptr) return;
	if (pitch < 0)
	{
		NLOGWARNING(niceName, "Send note on, pitch " << pitch << " is below 0. Please check your octave shift in the module's parameters");
		return;
	}

	if (logOutgoingData->boolValue()) NLOG(niceName, "Send Note on, channel : " << channel << ", note : " << MIDIManager::getNoteName(pitch) << " (pitch : " << String(pitch) + "), velocity : " << velocity);
	outActivityTrigger->trigger();
	withOutput([&](MIDIOutputDevice* d) { d->sendNoteOn(channel, pitch, velocity); });
}

void MIDIModule::sendNoteOff(int channel, int pitch)
{
	if (!enabled->boolValue()) return;
	if (outputDevice == nullptr) return;

	if (pitch < 0)
	{
		NLOGWARNING(niceName, "Send note off, pitch " << pitch << " is below 0. Please check your octave shift in the module's parameters");
		return;
	}

	if (logOutgoingData->boolValue()) NLOG(niceName, "Send Note off, channel : " << channel << ", note : " << MIDIManager::getNoteName(pitch) << " (pitch : " << String(pitch) + ")");
	outActivityTrigger->trigger();
	withOutput([&](MIDIOutputDevice* d) { d->sendNoteOff(channel, pitch); });
}

void MIDIModule::scheduleNoteOff(int channel, int pitch, double delayMs)
{
	if (pitch < 0) return;

	const ScopedLock sl(pendingNoteOffLock);
	const double due = Time::getMillisecondCounterHiRes() + jmax(0.0, delayMs);

	//a note played again while still held is extended : one note off, at the end, as before
	bool found = false;
	for (auto& p : pendingNoteOffs)
	{
		if (p.channel == channel && p.pitch == pitch)
		{
			p.dueMs = due;
			found = true;
		}
	}
	if (!found) pendingNoteOffs.add({ channel, pitch, due });

	//starting never waits for a callback in flight (only stopping does), so this is safe under the lock
	if (!noteOffTimer.isTimerRunning()) noteOffTimer.startTimer(1);
}

void MIDIModule::sendPendingNoteOffs(bool all)
{
	Array<PendingNoteOff> due;
	{
		const ScopedLock sl(pendingNoteOffLock);
		const double now = Time::getMillisecondCounterHiRes();
		for (int i = pendingNoteOffs.size() - 1; i >= 0; --i)
		{
			if (all || pendingNoteOffs.getReference(i).dueMs <= now)
			{
				due.insert(0, pendingNoteOffs[i]);
				pendingNoteOffs.remove(i);
			}
		}

		//stopping from the timer's own thread does not wait ; from another thread it waits for a callback in flight,
		//which needs this lock : only the timer stops itself
		if (pendingNoteOffs.isEmpty() && !all) noteOffTimer.stopTimer();
	}

	//completing a note that was played : not subject to the Enabled switch (disabling flushes these first, see
	//updateMIDIDevices), and silently nothing when there is no output any more
	for (auto& p : due)
	{
		if (logOutgoingData->boolValue()) NLOG(niceName, "Send Note off, channel : " << p.channel << ", note : " << MIDIManager::getNoteName(p.pitch) << " (pitch : " << String(p.pitch) + ")");
		withOutput([&](MIDIOutputDevice* d) { d->sendNoteOff(p.channel, p.pitch); });
	}
}

void MIDIModule::sendControlChange(int channel, int number, int value)
{
	if (!enabled->boolValue()) return;
	if (outputDevice == nullptr) return;
	if (logOutgoingData->boolValue()) NLOG(niceName, "Send Control Change, channel : " << channel << ", number : " << number << ", value " << value);
	outActivityTrigger->trigger();
	withOutput([&](MIDIOutputDevice* d) { d->sendControlChange(channel, number, value); });
}

void MIDIModule::sendProgramChange(int channel, int program)
{
	if (!enabled->boolValue()) return;
	if (outputDevice == nullptr) return;
	if (logOutgoingData->boolValue()) NLOG(niceName, "Send ProgramChange, channel : " << channel << ", program : " << program);
	outActivityTrigger->trigger();
	withOutput([&](MIDIOutputDevice* d) { d->sendProgramChange(channel, program); });
}

void MIDIModule::sendSysex(Array<uint8> data)
{
	if (!enabled->boolValue()) return;
	if (outputDevice == nullptr) return;
	if (logOutgoingData->boolValue())
	{
		String s = "Send Sysex " + String(data.size()) + " bytes : ";
		for (int i = 0; i < data.size(); ++i) s += "\n" + String(data[i]);
		NLOG(niceName, s);
	}
	outActivityTrigger->trigger();
	withOutput([&](MIDIOutputDevice* d) { d->sendSysEx(data); });
}

void MIDIModule::sendPitchWheel(int channel, int value)
{
	if (!enabled->boolValue()) return;
	if (outputDevice == nullptr) return;
	if (logOutgoingData->boolValue()) NLOG(niceName, "Send PitchWheel channel : " << channel << ", value : " << value);
	withOutput([&](MIDIOutputDevice* d) { d->sendPitchWheel(channel, value); });
}

void MIDIModule::sendChannelPressure(int channel, int value)
{
	if (!enabled->boolValue()) return;
	if (outputDevice == nullptr) return;
	if (logOutgoingData->boolValue()) NLOG(niceName, "Send Channel Pressure channel : " << channel << ", value : " << value);
	withOutput([&](MIDIOutputDevice* d) { d->sendChannelPressure(channel, value); });
}

void MIDIModule::sendAfterTouch(int channel, int note, int value)
{
	if (!enabled->boolValue()) return;
	if (outputDevice == nullptr) return;
	if (logOutgoingData->boolValue()) NLOG(niceName, "Send After touch channel : " << channel << ", note : " << note << ", value : " << value);
	withOutput([&](MIDIOutputDevice* d) { d->sendAfterTouch(channel, note, value); });
}

void MIDIModule::sendFullFrameTimecode(int hours, int minutes, int seconds, int frames, MidiMessage::SmpteTimecodeType timecodeType)
{
	if (!enabled->boolValue()) return;
	if (outputDevice == nullptr) return;
	if (logOutgoingData->boolValue())
	{
		NLOG(niceName, "Send full frame timecode : " << hours << ":" << minutes << ":" << seconds << "." << frames << " / " << timecodeType);
	}
	withOutput([&](MIDIOutputDevice* d) { d->sendFullframeTimecode(hours, minutes, seconds, frames, timecodeType); });
}

void MIDIModule::sendMidiMachineControlCommand(MidiMessage::MidiMachineControlCommand command)
{
	if (!enabled->boolValue()) return;
	if (outputDevice == nullptr) return;
	if (logOutgoingData->boolValue())
	{
		NLOG(niceName, "Send midi machine control command : " << command);
	}
	withOutput([&](MIDIOutputDevice* d) { d->sendMidiMachineControlCommand(command); });
}

void MIDIModule::sendMidiMachineControlGoto(int hours, int minutes, int seconds, int frames)
{
	if (!enabled->boolValue()) return;
	if (outputDevice == nullptr) return;
	if (logOutgoingData->boolValue())
	{
		NLOG(niceName, "Send midi machine control goto : " << hours << ":" << minutes << ":" << seconds << "." << frames);
	}
	withOutput([&](MIDIOutputDevice* d) { d->sendMidiMachineControlGoto(hours, minutes, seconds, frames); });
}


void MIDIModule::onControllableFeedbackUpdateInternal(ControllableContainer* cc, Controllable* c)
{
	Module::onControllableFeedbackUpdateInternal(cc, c);

	if (c == midiParam)
	{
		updateMIDIDevices();
	}


	if (autoFeedback->boolValue())
	{
		if (isControllableInValuesContainer(c))
		{
			MIDIValueParameter* mvp = dynamic_cast<MIDIValueParameter*>(c);
			if (mvp != nullptr)
			{
				switch (mvp->type)
				{
				case MIDIValueParameter::NOTE_ON: sendNoteOn(mvp->channel, mvp->pitchOrNumber, mvp->intValue()); break;
				case MIDIValueParameter::NOTE_OFF: sendNoteOff(mvp->channel, mvp->pitchOrNumber); break;
				case MIDIValueParameter::CONTROL_CHANGE: sendControlChange(mvp->channel, mvp->pitchOrNumber, mvp->intValue()); break;
				case MIDIValueParameter::PITCH_WHEEL: sendPitchWheel(mvp->channel, mvp->intValue()); break;
				case MIDIValueParameter::CHANNEL_PRESSURE: sendChannelPressure(mvp->channel, mvp->intValue()); break;
				case MIDIValueParameter::AFTER_TOUCH: sendAfterTouch(mvp->channel, mvp->pitchOrNumber, mvp->intValue()); break;
				case MIDIValueParameter::PROGRAM_CHANGE: sendProgramChange(mvp->channel, mvp->intValue()); break;

				default:
					break;
				}
			}
		}
	}

	//not an "else" of Auto Feedback : with Auto Feedback on, turning Send Clock off never stopped the clock thread
	if (c == sendClock)
	{
		outClock.stop();

		if (sendClock->boolValue())
		{
			outClock.setOutDevice(outputDevice);
			outClock.setBPM(bpm->floatValue());
			outClock.start();
		}
	}

	if (sendClock->boolValue())
	{
		if (c == bpm) outClock.setBPM(bpm->floatValue());
		else if (c == midiStartTrigger)
		{
			outClock.stop();
			outClock.start();
		}
		else if (c == midiStopTrigger)
		{
			outClock.stop();
		}
		else if (c == midiContinueTrigger)
		{
			outClock.start();
		}
	}
}

void MIDIModule::onContainerParameterChangedInternal(Parameter* p)
{
	Module::onContainerParameterChangedInternal(p);
	if (p == enabled){
		updateMIDIDevices();
	}
}

void MIDIModule::updateMIDIDevices()
{
	MIDIInputDevice* newInput = nullptr;
	MIDIOutputDevice* newOutput = nullptr;
	if (enabled->boolValue()){
		newInput = midiParam->inputDevice;
		newOutput = midiParam->outputDevice;
	}

	if (newInput != inputDevice)
	{
		if (inputDevice != nullptr) inputDevice->removeMIDIInputListener(this); //waits for a message being dispatched
		mtcReceiver.reset();
		mtcIsPlaying->setValue(false);
		inputDevice = newInput;

		if (inputDevice != nullptr)
		{
			inputDevice->addMIDIInputListener(this);
			mtcReceiver.reset(new MTCReceiver(inputDevice));
			mtcReceiver->addMTCListener(this);
			noteOns.clear();
		}
	}

	if (newOutput != outputDevice)
	{
		//notes a Full Note still owes go to the output that played them, before it goes
		sendPendingNoteOffs(true);

		MIDIOutputDevice* oldOutput = nullptr;
		{
			const ScopedLock sl(outputLock); //a send in flight finishes first, the next one sees the new device
			oldOutput = outputDevice;
			outputDevice = newOutput;
		}

		//the clock thread holds the device too, running or not : MIDIManager deletes an unplugged one right after this
		outClock.setOutDevice(outputDevice);

		if (oldOutput != nullptr) oldOutput->close();
		if (outputDevice != nullptr) outputDevice->open();
	}


	setupIOConfiguration(inputDevice != nullptr || valuesCC.controllables.size() > 0, outputDevice != nullptr);

	isConnected->setValue(inputDevice != nullptr || outputDevice != nullptr);
}

void MIDIModule::midiMessageReceived(const MidiMessage& msg)
{
	//the MIDI driver's thread : queue only
	{
		const ScopedLock sl(inputQueueLock);
		if ((int)inputQueue.size() >= 10000)
		{
			droppedInput++;
			return;
		}
		inputQueue.push_back(msg);
	}
	inputQueueUpdater.triggerAsyncUpdate();
}

void MIDIModule::handleQueuedInput()
{
	//In arrival order, for at most 20 ms a pass : the rest waits for the next pass, so a flood never holds the message
	//thread (a value added by Auto Add re-sorts the whole list, and a batch of 10 000 took tens of seconds, during which
	//the interface, a project load and even closing the window all waited)
	const uint32 start = Time::getMillisecondCounter();
	WeakReference<Inspectable> self(this); //a script reacting to a message could remove this module
	bool drained = false;
	for (;;)
	{
		if (isClearing) return;

		MidiMessage m;
		{
			const ScopedLock sl(inputQueueLock);
			drained = inputQueue.empty();
			if (drained) break;
			m = std::move(inputQueue.front());
			inputQueue.pop_front();
		}

		dispatchInput(m);
		if (self.wasObjectDeleted()) return;

		if (Time::getMillisecondCounter() - start >= 20)
		{
			inputQueueUpdater.triggerAsyncUpdate();
			break;
		}
	}

	//at most one warning every 5 s during a flood, and the rest once it is over
	int dropped = 0;
	const uint32 now = Time::getMillisecondCounter();
	if (drained || now - lastDropLog >= 5000)
	{
		const ScopedLock sl(inputQueueLock);
		dropped = droppedInput;
		droppedInput = 0;
	}

	if (dropped > 0)
	{
		lastDropLog = now;
		NLOGWARNING(niceName, "Dropped " << dropped << " incoming MIDI message(s) : they arrived faster than they could be handled");
	}
}

void MIDIModule::dispatchInput(const MidiMessage& message)
{
	//message thread, in arrival order, and sorted as MIDIInputDevice sorts them
	handleThru(message);

	if (message.isNoteOn()) handleNoteOn(message.getChannel(), message.getNoteNumber(), message.getVelocity());
	else if (message.isNoteOff()) handleNoteOff(message.getChannel(), message.getNoteNumber(), 0); //force note off to velocity 0
	else if (message.isController()) handleControlChange(message.getChannel(), message.getControllerNumber(), message.getControllerValue());
	else if (message.isProgramChange()) handleProgramChange(message.getChannel(), message.getProgramChangeNumber());
	else if (message.isFullFrame()) handleFullFrameTimecode(message);
	else if (message.isQuarterFrame()) {} //the MTC receiver's, on the driver's thread
	else if (message.isPitchWheel()) handlePitchWheel(message.getChannel(), message.getPitchWheelValue());
	else if (message.isChannelPressure()) handleChannelPressure(message.getChannel(), message.getChannelPressureValue());
	else if (message.isAftertouch()) handleAfterTouch(message.getChannel(), message.getNoteNumber(), message.getAfterTouchValue());
	else if (message.isMidiClock())
	{
		if (enabled->boolValue()) inActivityTrigger->trigger(); //the tempo itself was measured on arrival
	}
	else if (message.isMidiStart()) handleMidiStart();
	else if (message.isMidiStop()) handleMidiStop();
	else if (message.isMidiContinue()) handleMidiContinue();
	else if (message.isMidiMachineControlMessage()) handleMidiMachineControlCommand(message.getMidiMachineControlCommand());
	else if (message.isSysEx()) handleSysEx(message);
	else
	{
		int hours, minutes, seconds, frames;
		if (message.isMidiMachineControlGoto(hours, minutes, seconds, frames)) handleMidiMachineControlGoto(hours, minutes, seconds, frames);
	}
}

void MIDIModule::handleNoteOn(int channel, int pitch, int velocity)
{
	if (!enabled->boolValue() && !manualAddMode) return;
	inActivityTrigger->trigger();

	String noteName = usePitchForNoteNames->boolValue() ? "Pitch " + String(pitch) : MIDIManager::getNoteName(pitch, true, octaveShift->intValue());

	if (logIncomingData->boolValue())  NLOG(niceName, "Note On : " << channel << ", " << noteName << " ( pitch : " + String(pitch) + " ), " << velocity);

	
	noteOns.addIfNotAlreadyThere(channel * 128 + pitch);

	if (useGenericControls) updateValue(channel, noteName, velocity, MIDIValueParameter::NOTE_ON, pitch);

	//moved after updateValue so "learn" will work on actual notes and CC, not on "last*" parameters.
	lastChannel->setValue(channel);
	lastPitch->setValue(pitch);
	lastVelocity->setValue(velocity);
	oneNoteOn->setValue(true);
	notePlayed->trigger();

	if (scriptManager->items.size() > 0) scriptManager->callFunctionOnAllItems(noteOnEventId, Array<var>(channel, pitch, velocity));
}

void MIDIModule::handleNoteOff(int channel, int pitch, int velocity)
{
	if (!enabled->boolValue() && !manualAddMode) return;
	inActivityTrigger->trigger();

	noteOns.removeAllInstancesOf(channel * 128 + pitch);
	if (noteOns.isEmpty()) oneNoteOn->setValue(false);

	String noteName = usePitchForNoteNames->boolValue() ? "Pitch " + String(pitch) : MIDIManager::getNoteName(pitch, true, octaveShift->intValue());

	if (logIncomingData->boolValue()) NLOG(niceName, "Note Off : " << channel << ", " << noteName << " ( pitch : " + String(pitch) + " ), " << velocity);

	if (useGenericControls) updateValue(channel, noteName, velocity, MIDIValueParameter::NOTE_OFF, pitch);

	if (scriptManager->items.size() > 0) scriptManager->callFunctionOnAllItems(noteOffEventId, Array<var>(channel, pitch, velocity));

}

void MIDIModule::handleControlChange(int channel, int number, int value)
{
	if (!enabled->boolValue() && !manualAddMode) return;
	inActivityTrigger->trigger();
	if (logIncomingData->boolValue()) NLOG(niceName, "Control Change : " << channel << ", " << number << ", " << value);

	if (useGenericControls) updateValue(channel, "CC" + String(number), value, MIDIValueParameter::CONTROL_CHANGE, number);

	if (scriptManager->items.size() > 0) scriptManager->callFunctionOnAllItems(ccEventId, Array<var>(channel, number, value));

}

void MIDIModule::handleProgramChange(int channel, int value)
{
	if (!enabled->boolValue() && !manualAddMode) return;
	inActivityTrigger->trigger();
	if (logIncomingData->boolValue()) NLOG(niceName, "Program Change : " << channel << ", " << value);

	if (useGenericControls) updateValue(channel, "ProgramChange", value, MIDIValueParameter::PROGRAM_CHANGE, 0);

	if (scriptManager->items.size() > 0) scriptManager->callFunctionOnAllItems(programChangeId, Array<var>(channel, value));
}

void MIDIModule::handleSysEx(const MidiMessage& msg)
{
	if (!enabled->boolValue() && !manualAddMode) return;
	inActivityTrigger->trigger();

	const auto* sysExDataPtr = msg.getSysExData();
	const int sysExSize = msg.getSysExDataSize();
	if (sysExDataPtr == nullptr || sysExSize <= 0) return;

	Array<uint8> data(sysExDataPtr, sysExSize);



	if (logIncomingData->boolValue())
	{
		String log = "Sysex received, " + String(data.size()) + " bytes";
		for (int i = 0; i < data.size(); ++i)
		{
			log += "\n" + String(data[i]);
		}
		NLOG(niceName, log);
	}


	var sysexData;
	for (auto& d : data) sysexData.append(d);
	Array<var> args;
	args.add(sysexData);
	scriptManager->callFunctionOnAllItems(sysexEventId, args);
}

void MIDIModule::handleFullFrameTimecode(const MidiMessage& msg)
{
	if (!enabled->boolValue()) return;
	inActivityTrigger->trigger();

	int hours = 0, minutes = 0, seconds = 0, frames = 0;
	MidiMessage::SmpteTimecodeType timecodeType;
	msg.getFullFrameParameters(hours, minutes, seconds, frames, timecodeType);


	if (logIncomingData->boolValue())
	{
		NLOG(niceName, "Full frame timecode received : " << hours << ":" << minutes << ":" << seconds << "." << frames << " / " << timecodeType);
	}
}

void MIDIModule::handlePitchWheel(int channel, int value)
{
	if (!enabled->boolValue() && !manualAddMode) return;
	inActivityTrigger->trigger();
	if (logIncomingData->boolValue()) NLOG(niceName, "Pitch wheel, channel : " << channel << ", value : " << value);

	if (useGenericControls) updateValue(channel, "PitchWheel", value, MIDIValueParameter::PITCH_WHEEL, 0);

	if (scriptManager->items.size() > 0) scriptManager->callFunctionOnAllItems(pitchWheelEventId, Array<var>(channel, value));
}

void MIDIModule::handleChannelPressure(int channel, int value)
{
	if (!enabled->boolValue() && !manualAddMode) return;
	inActivityTrigger->trigger();
	if (logIncomingData->boolValue()) NLOG(niceName, "Channel Pressure, channel : " << channel << ", value : " << value);

	if (useGenericControls) updateValue(channel, "ChannelPressure", value, MIDIValueParameter::CHANNEL_PRESSURE, 0);

	if (scriptManager->items.size() > 0) scriptManager->callFunctionOnAllItems(channelPressureId, Array<var>(channel, value));
}

void MIDIModule::handleAfterTouch(int channel, int note, int value)
{
	if (!enabled->boolValue() && !manualAddMode) return;
	inActivityTrigger->trigger();
	if (logIncomingData->boolValue()) NLOG(niceName, "After Touch, channel : " << channel << ", note : " << note << ", value : " << value);

	if (useGenericControls) updateValue(channel, "AfterTouch " + (usePitchForNoteNames->boolValue() ? "Pitch " + String(note) : MIDIManager::getNoteName(note)), value, MIDIValueParameter::AFTER_TOUCH, note);

	if (scriptManager->items.size() > 0) scriptManager->callFunctionOnAllItems(afterTouchId, Array<var>(channel, note, value));
}

void MIDIModule::handleThru(const MidiMessage& msg)
{
	if (!enabled->boolValue()) return;

	if (thruManager != nullptr)
	{
		for (auto& c : thruManager->controllables)
		{
			if (TargetParameter* mt = dynamic_cast<TargetParameter*>(c))
			{
				if (!mt->enabled) continue;
				if (MIDIModule* m = dynamic_cast<MIDIModule*>(mt->targetContainer.get()))
				{
					//queued on that device, and dropped if it is not open
					if (m->midiParam->outputDevice != nullptr) m->midiParam->outputDevice->sendMessageNow(msg);
				}
			}
		}
	}
}

void MIDIModule::midiClockReceived()
{
	//the MIDI driver's thread : the tempo is measured on arrival, and only a parameter is set from here (notified on
	//the message thread). The activity blink comes with the queued message.
	if (!enabled->boolValue()) return;

	//if (logIncomingData->boolValue())
	//{
	//	NLOG(niceName, "MIDI Clock received");
	//}



	//for (int i = 0; i < 24; i++)
	//{
	//	int prev = i == 0 ? 23 : i - 1;
	//	double diff = lastClockReceiveTimes[i] - lastClockReceiveTimes[prev];
	//	targetBPM += diff;
	//	DBG("Target BPM Here (" << i << ") : " << targetBPM);
	//}

	//DBG("Target BPM end of loop : " << targetBPM);
	//targetBPM /= 24.0;

	double t = Time::getMillisecondCounterHiRes() / 1000.0;
	double diff = jmax<double>(0, t - lastClockReceiveTime);
	clockDeltaTimes[lastClockReceiveTimeIndex] = diff;

	if (lastClockReceiveTimeIndex == 0)
	{
		double quarterNoteDiff = 0;
		for (int i = 0; i < 24; i++) quarterNoteDiff += clockDeltaTimes[i];
		if (quarterNoteDiff > 0)
		{
			double targetBPM = 60.0 / quarterNoteDiff;
			bpm->setValue(targetBPM);
		}
	}

	lastClockReceiveTimeIndex = (lastClockReceiveTimeIndex + 1) % 24;
	lastClockReceiveTime = t;

}

void MIDIModule::handleMidiStart()
{
	if (!enabled->boolValue()) return;
	inActivityTrigger->trigger();
	if (logIncomingData->boolValue())
	{
		NLOG(niceName, "MIDI Start received");
	}
	midiStartTrigger->trigger();
}

void MIDIModule::handleMidiStop()
{
	if (!enabled->boolValue()) return;
	inActivityTrigger->trigger();
	if (logIncomingData->boolValue())
	{
		NLOG(niceName, "MIDI Stop received");
	}
	midiStopTrigger->trigger();
}

void MIDIModule::handleMidiContinue()
{
	if (!enabled->boolValue()) return;
	inActivityTrigger->trigger();
	if (logIncomingData->boolValue())
	{
		NLOG(niceName, "MIDI Continue received");
	}
	midiContinueTrigger->trigger();
}

void MIDIModule::handleMidiMachineControlCommand(MidiMessage::MidiMachineControlCommand type)
{
	if (!enabled->boolValue() && !manualAddMode) return;
	inActivityTrigger->trigger();
	if (logIncomingData->boolValue()) NLOG(niceName, "MMC Command : " << (int)type);

	if (scriptManager->items.size() > 0) scriptManager->callFunctionOnAllItems(machineControlCommandId, Array<var>((int)type));
}

void MIDIModule::handleMidiMachineControlGoto(int hours, int minutes, int seconds, int frames)
{
	if (!enabled->boolValue() && !manualAddMode) return;
	inActivityTrigger->trigger();
	if (logIncomingData->boolValue()) NLOG(niceName, "MMC Goto : " << hours << ":" << minutes << ":" << seconds << "." << frames);

	if (scriptManager->items.size() > 0) scriptManager->callFunctionOnAllItems(machineControlGotoId, Array<var>(hours, minutes, seconds, frames));
}

void MIDIModule::mtcStarted()
{
	mtcIsPlaying->setValue(true);
}

void MIDIModule::mtcStopped()
{
	mtcIsPlaying->setValue(false);
}

void MIDIModule::mtcTimeUpdated(bool isFullFrame)
{
	//if(isFullFrame) mtcIsPlaying->setValue(true);
	if (mtcReceiver != nullptr) mtcTime->setValue(mtcReceiver->getTime());
}


var MIDIModule::sendNoteOnFromScript(const var::NativeFunctionArgs& args)
{
	MIDIModule* m = getObjectFromJS<MIDIModule>(args);
	if (!m->enabled->boolValue()) return var();
	if (!checkNumArgs(m->niceName, args, 3)) return var();


	m->sendNoteOn(args.arguments[0], args.arguments[1], args.arguments[2]);
	return var();
}

var MIDIModule::sendNoteOffFromScript(const var::NativeFunctionArgs& args)
{
	MIDIModule* m = getObjectFromJS<MIDIModule>(args);
	if (!m->enabled->boolValue()) return var();
	if (!checkNumArgs(m->niceName, args, 2)) return var();


	m->sendNoteOff(args.arguments[0], args.arguments[1]);
	return var();
}

var MIDIModule::sendCCFromScript(const var::NativeFunctionArgs& args)
{
	MIDIModule* m = getObjectFromJS<MIDIModule>(args);
	if (!m->enabled->boolValue()) return var();
	if (!checkNumArgs(m->niceName, args, 3)) return var();

	m->sendControlChange(args.arguments[0], args.arguments[1], args.arguments[2]);
	return var();
}

var MIDIModule::sendSysexFromScript(const var::NativeFunctionArgs& args)
{
	MIDIModule* m = getObjectFromJS<MIDIModule>(args);
	if (!m->enabled->boolValue()) return var();
	if (!checkNumArgs(m->niceName, args, 1)) return var();

	Array<const var*> allArgs;
	for (int i = 0; i < args.numArguments; ++i)
	{
		if (args.arguments[i].isArray())
		{
			for (int j = 0; j < args.arguments[i].size(); j++)
			{
				allArgs.add(&args.arguments[i][j]);
			}
		}
		else
		{
			allArgs.add(&args.arguments[i]);
		}
	}

	int numArgs = allArgs.size();

	Array<uint8> data;
	for (int i = 0; i < numArgs; ++i)
	{
		const var* a = allArgs[i];
		if (a->isInt() || a->isInt64() || a->isDouble() || a->isBool())data.add((uint8)(int)*a);
		else if (a->isString())
		{
			String s = a->toString();
			for (int j = 0; j < s.length(); j++)
			{
				data.add((uint8)s[j]);
			}
		}
	}
	m->sendSysex(data);

	return var();
}

var MIDIModule::sendProgramChangeFromScript(const var::NativeFunctionArgs& args)
{
	MIDIModule* m = getObjectFromJS<MIDIModule>(args);
	if (!m->enabled->boolValue()) return var();
	if (!checkNumArgs(m->niceName, args, 2)) return var();


	m->sendProgramChange(args.arguments[0], args.arguments[1]);
	return var();
}

var MIDIModule::sendPitchWheelFromScript(const var::NativeFunctionArgs& args)
{
	MIDIModule* m = getObjectFromJS<MIDIModule>(args);
	if (!m->enabled->boolValue()) return var();
	if (!checkNumArgs(m->niceName, args, 2)) return var();


	m->sendPitchWheel(args.arguments[0], args.arguments[1]);
	return var();
}

var MIDIModule::sendChannelPressureFromScript(const var::NativeFunctionArgs& args)
{
	MIDIModule* m = getObjectFromJS<MIDIModule>(args);
	if (!m->enabled->boolValue()) return var();
	if (!checkNumArgs(m->niceName, args, 2)) return var();
	m->sendChannelPressure(args.arguments[0], args.arguments[1]);
	return var();
}

var MIDIModule::sendAfterTouchFromScript(const var::NativeFunctionArgs& args)
{
	MIDIModule* m = getObjectFromJS<MIDIModule>(args);
	if (!m->enabled->boolValue()) return var();
	if (!checkNumArgs(m->niceName, args, 3)) return var();

	m->sendAfterTouch(args.arguments[0], args.arguments[1], args.arguments[2]);
	return var();
}

var MIDIModule::sendMidiMachineControlCommandFromScript(const var::NativeFunctionArgs& args)
{
	MIDIModule* m = getObjectFromJS<MIDIModule>(args);
	if (!m->enabled->boolValue()) return var();
	if (!checkNumArgs(m->niceName, args, 1)) return var();

	MidiMessage::MidiMachineControlCommand type = (MidiMessage::MidiMachineControlCommand)(int)args.arguments[0];

	m->sendMidiMachineControlCommand(type);
	return var();
}

var MIDIModule::sendMidiMachineControlGotoFromScript(const var::NativeFunctionArgs& args)
{
	MIDIModule* m = getObjectFromJS<MIDIModule>(args);
	if (!m->enabled->boolValue()) return var();
	if (!checkNumArgs(m->niceName, args, 4)) return var();

	m->sendMidiMachineControlGoto(args.arguments[0], args.arguments[1], args.arguments[2], args.arguments[3]);
	return var();
}

void MIDIModule::updateValue(const int& channel, const String& n, const int& val, const MIDIValueParameter::Type& type, const int& pitchOrNumber)
{
	ControllableContainer* cParentContainer = &valuesCC;

	String pName = n;

	if (useHierarchy->boolValue())
	{
		ControllableContainer* channelContainer = valuesCC.getControllableContainerByName("Channel " + String(channel), true);
		if (channelContainer == nullptr)
		{
			if (!autoAdd->boolValue()) return;

			channelContainer = new ControllableContainer("Channel " + String(channel));
			channelContainer->saveAndLoadRecursiveData = true;
			channelContainer->isRemovableByUser = true;
			channelContainer->customControllableComparator = &midiValueComparator;
			valuesCC.addChildControllableContainer(channelContainer, true);
		}

		String typeName;
		switch (type)
		{
		case MIDIValueParameter::NOTE_ON:
		case MIDIValueParameter::NOTE_OFF:
			typeName = "Notes";
			break;

		case MIDIValueParameter::CONTROL_CHANGE:
			typeName = "Control Change";
			break;

		case MIDIValueParameter::PROGRAM_CHANGE:
			typeName = "Program Change";
			break;

		default:
			typeName = "Other";
		}

		ControllableContainer* typeContainer = channelContainer->getControllableContainerByName(typeName, true);
		if (typeContainer == nullptr)
		{
			if (!autoAdd->boolValue()) return;

			typeContainer = new ControllableContainer(typeName);
			typeContainer->saveAndLoadRecursiveData = true;
			typeContainer->isRemovableByUser = true;
			typeContainer->customControllableComparator = &MIDIModule::midiValueComparator;
			channelContainer->addChildControllableContainer(typeContainer, true);
		}

		cParentContainer = typeContainer;
	}
	else
	{
		pName = "[" + String(channel) + "] " + n;
	}

	Parameter* p = dynamic_cast<Parameter*>(cParentContainer->getControllableByName(pName, true));

	if (p == nullptr)
	{
		if (autoAdd->boolValue() || manualAddMode)
		{
			p = new MIDIValueParameter(pName, "Channel " + String(channel) + " : " + n, 0, channel, pitchOrNumber, type);
			p->setValue(val);
			p->isRemovableByUser = true;
			p->saveValueOnly = false;
			cParentContainer->addParameter(p);
			cParentContainer->sortControllables();
		}
	}
	else if (!manualAddMode)
	{
		p->setValue(val);
	}

}

void MIDIModule::showMenuAndCreateValue(ControllableContainer* container)
{
	MIDIModule* module = dynamic_cast<MIDIModule*>(container->parentContainer.get());
	if (module == nullptr) return;

	PopupMenu m;
	m.addItem(1, "Add Note");
	m.addItem(2, "Add Control Change");
	m.addItem(3, "Add Pitch Wheel");
	m.addItem(4, "Add Channel Pressure");
	m.addItem(5, "Add After Touch");
	m.addItem(6, "Add Program Change");

	m.showMenuAsync(PopupMenu::Options(), [module](int mResult)
		{
			if (mResult == 0) return;

			String mType = mResult == 1 ? "Note" : mResult == 3 ? "Pitch Wheel" : mResult == 6 ? "Program Change" : "Control Change";

			AlertWindow* window = new AlertWindow("Add a " + mType, "Configure the parameters for this " + mType, AlertWindow::AlertIconType::NoIcon);
			window->addTextEditor("channel", "1", "Channel (1-16)");

			if (mResult != 3 && mResult != 4 && mResult != 6)
			{
				String mName = mResult == 1 ? "Pitch" : "Number";
				window->addTextEditor("pitch", "1", mName + "(0-127)");
			}

			window->addButton("OK", 1, KeyPress(KeyPress::returnKey));
			window->addButton("Cancel", 0, KeyPress(KeyPress::escapeKey));

			window->enterModalState(true, ModalCallbackFunction::create([module, window, mResult](int result)
				{
					if (result)
					{
						int channel = jlimit<int>(1, 16, window->getTextEditorContents("channel").getIntValue());

						if (mResult == 3)
						{
							module->handlePitchWheel(channel, 0);
						}
						else if (mResult == 4)
						{
							module->handleChannelPressure(channel, 0);
						}
						else if (mResult == 5)
						{
							module->handleAfterTouch(channel, window->getTextEditorContents("pitch").getIntValue(), 0);
						}
						else if (mResult == 6)
						{
							module->handleProgramChange(channel, 0);
						}
						else
						{
							int pitch = jlimit<int>(0, 127, window->getTextEditorContents("pitch").getIntValue());

							module->manualAddMode = true;
							if (mResult == 1) module->handleNoteOn(channel, pitch, 0);
							else module->handleControlChange(channel, pitch, 0);
							module->manualAddMode = false;
						}
					}
				}),
				true
			);
		}
	);
}

void MIDIModule::createThruControllable(ControllableContainer* cc)
{
	TargetParameter* p = new TargetParameter(cc->getUniqueNameInContainer("Output module 1"), "Target module to send the raw data to", "");
	p->targetType = TargetParameter::CONTAINER;
	p->customGetTargetContainerFunc = &ModuleManager::showAndGetModuleOfType<MIDIModule>;
	p->isRemovableByUser = true;
	p->canBeDisabledByUser = true;
	p->saveValueOnly = false;
	cc->addParameter(p);
}

void MIDIModule::loadJSONDataInternal(var data)
{
	Module::loadJSONDataInternal(data);

	Array<WeakReference<ControllableContainer>> valueContainers = valuesCC.getAllContainers(true);

	for (auto& c : valueContainers)
	{
		if (c == &tempoCC || c == &infoCC) continue;
		c->customControllableComparator = &midiValueComparator;
		c->sortControllables();
	}

	valuesCC.sortControllables();

	setupIOConfiguration(inputDevice != nullptr || valuesCC.controllables.size() > 0, outputDevice != nullptr);

	if (thruManager != nullptr)
	{
		//thruManager->loadJSONData(data.getProperty("thru", var()));
		for (auto& c : thruManager->controllables)
		{
			if (TargetParameter* mt = dynamic_cast<TargetParameter*>(c))
			{
				mt->targetType = TargetParameter::CONTAINER;
				mt->customGetTargetContainerFunc = &ModuleManager::showAndGetModuleOfType<MIDIModule>;
				mt->isRemovableByUser = true;
				mt->canBeDisabledByUser = true;
			}
		}
	}
}


MIDIModule::MIDIRouteParams::MIDIRouteParams(Module* sourceModule, Controllable* c) :
	type(nullptr),
	channel(nullptr),
	pitchOrNumber(nullptr)
{
	if (c->type != Controllable::FLOAT && c->type != Controllable::INT && c->type != Controllable::BOOL)
	{
		NLOGWARNING(niceName, c->niceName + " is of type " + c->getTypeString() + ",\nOnly float and integers can be routed through MIDI");
		return;
	}

	type = addEnumParameter("Type", "The type of MIDI Command to route");
	type->addOption("Control Change", MIDIManager::CONTROL_CHANGE)->addOption("Note On", MIDIManager::NOTE_ON)->addOption("Note Off", MIDIManager::NOTE_OFF)->addOption("Pitch Wheel", MIDIManager::PITCH_WHEEL);
	type->setValueWithData(MIDIManager::getInstance()->midiRouterDefaultType->getValueData());

	channel = addIntParameter("Channel", "The Channel", 1, 1, 16);

	if (type->getValueDataAsEnum<MIDIManager::MIDIEventType>() != MIDIManager::PITCH_WHEEL)
	{
		pitchOrNumber = addIntParameter("Pitch / Number", "Pitch if type is a note, number if it is a controlChange", 0, 0, 127);
	}
}

void MIDIModule::MIDIRouteParams::onContainerParameterChanged(Parameter* p)
{
	RouteParams::onContainerParameterChanged(p);

	if (p == type)
	{
		if (type->getValueDataAsEnum<MIDIManager::MIDIEventType>() != MIDIManager::PITCH_WHEEL)
		{
			if (pitchOrNumber == nullptr) pitchOrNumber = addIntParameter("Pitch / Number", "Pitch if type is a note, number if it is a controlChange", 0, 0, 127);
		}
		else
		{
			if (pitchOrNumber != nullptr)
			{
				removeControllable(pitchOrNumber);
				pitchOrNumber = nullptr;
			}
		}
	}
}


void MIDIModule::handleRoutedModuleValue(Controllable* c, RouteParams* p)
{
	if (c == nullptr || p == nullptr) return;

	if (MIDIRouteParams* mp = dynamic_cast<MIDIRouteParams*>(p))
	{
		if (mp->type == nullptr) return;

		MIDIManager::MIDIEventType t = mp->type->getValueDataAsEnum<MIDIManager::MIDIEventType>();

		int value = 0;

		Parameter* sp = c->type == Controllable::TRIGGER ? nullptr : dynamic_cast<Parameter*>(c);
		if (sp != nullptr)
		{
			if (sp->hasRange()) value = (float)sp->getNormalizedValue() * 127;
			else value = jlimit(0, 127, sp->intValue());
		}

		switch (t)
		{
		case MIDIManager::NOTE_ON:
			sendNoteOn(mp->channel->intValue(), mp->pitchOrNumber->intValue(), value);
			break;

		case MIDIManager::NOTE_OFF:
			sendNoteOff(mp->channel->intValue(), mp->pitchOrNumber->intValue());
			break;

		case MIDIManager::CONTROL_CHANGE:
			sendControlChange(mp->channel->intValue(), mp->pitchOrNumber->intValue(), value);
			break;

		case MIDIManager::PITCH_WHEEL:
			sendPitchWheel(mp->channel->intValue(), value);
			break;

		default:
			break;
		}
	}
}

MIDIModule::MIDIModuleRouterController::MIDIModuleRouterController(ModuleRouter* router) :
	ModuleRouterController(router)
{
	setAllCC = addTrigger("Set All ControlChange", "Auto set all to control change");
	setAllNote = addTrigger("Set All Notes", "Auto set all to notes");
	autoSetPitch = addTrigger("Auto-set pitch", "Auto set all notes and controlChange to incrementing pitch/number");
}

void MIDIModule::MIDIModuleRouterController::triggerTriggered(Trigger* t)
{
	if (t == setAllCC || t == setAllNote)
	{
		MIDIManager::MIDIEventType midiType = t == setAllCC ? MIDIManager::CONTROL_CHANGE : MIDIManager::NOTE_ON;

		for (auto& mrv : router->sourceValues.items)
		{
			if (MIDIRouteParams* dp = dynamic_cast<MIDIRouteParams*>(mrv->routeParams.get()))
			{
				if (dp->type != nullptr) dp->type->setValueWithData(midiType); //only MIDIRouteParams with type
			}
		}
	}
	else if (t == autoSetPitch)
	{
		int startIndex = 0;
		for (auto& mrv : router->sourceValues.items)
		{
			if (MIDIRouteParams* dp = dynamic_cast<MIDIRouteParams*>(mrv->routeParams.get()))
			{
				if (dp->pitchOrNumber != nullptr) dp->pitchOrNumber->setValue(startIndex++);
			}
		}
	}
}

int MIDIValueComparator::compareElements(Controllable* c1, Controllable* c2)
{
	MIDIValueParameter* m1 = dynamic_cast<MIDIValueParameter*>(c1);
	MIDIValueParameter* m2 = dynamic_cast<MIDIValueParameter*>(c2);
	if (m1 == nullptr || m2 == nullptr) return 0;

	int v1 = m1->channel * 10000 + m1->type * 1000 + m1->pitchOrNumber;
	int v2 = m2->channel * 10000 + m2->type * 1000 + m2->pitchOrNumber;

	if (v1 > v2) return 1;
	else if (v1 < v2) return -1;
	return 0;
}
