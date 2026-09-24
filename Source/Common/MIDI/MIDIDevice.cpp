#include "MIDIDevice.h"
/*
  ==============================================================================

	MIDIDevice.cpp
	Created: 20 Dec 2016 1:17:56pm
	Author:  Ben

  ==============================================================================
*/

MIDIDevice::MIDIDevice(const MidiDeviceInfo& info, Type t) :
	id(info.identifier),
	name(info.name),
	type(t)
{}



MIDIInputDevice::MIDIInputDevice(const MidiDeviceInfo& info) :
	MIDIDevice(info, MIDI_IN)
{
}

MIDIInputDevice::~MIDIInputDevice()
{
}

void MIDIInputDevice::addMIDIInputListener(MIDIInputListener* newListener)
{
	bool shouldOpen = false;
	{
		const ScopedLock sl(listenerLock);
		inputListeners.add(newListener);
		shouldOpen = inputListeners.size() == 1 && device == nullptr;
	}
	if (!shouldOpen) return;

	std::unique_ptr<MidiInput> d = MidiInput::openDevice(id, this);
	if (d == nullptr)
	{
		LOG("MIDI In " << name << " open error !");
		return;
	}

	MidiInput* opened = d.get();
	{
		const ScopedLock sl(listenerLock);
		device = std::move(d);
	}
	opened->start();
	LOG("MIDI In " << opened->getName() << " opened");
}

void MIDIInputDevice::removeMIDIInputListener(MIDIInputListener* listener)
{
	std::unique_ptr<MidiInput> toClose;
	{
		const ScopedLock sl(listenerLock); //waits for a message being dispatched to finish
		inputListeners.remove(listener);
		if (inputListeners.size() == 0) toClose = std::move(device);
	}

	//stopped outside the lock : stop() can wait for a driver callback, and that callback waits for the lock
	if (toClose != nullptr)
	{
		toClose->stop();
		toClose.reset();
		LOG("MIDI In " << name << " closed");
	}
}

void MIDIInputDevice::handleIncomingMidiMessage(MidiInput* source, const MidiMessage& message)
{
	const ScopedLock sl(listenerLock);

	if (source != device.get())
	{
		DBG("different device");
		return;
	}

	inputListeners.call(&MIDIInputListener::midiMessageReceived, message);

	if (message.isNoteOn()) inputListeners.call(&MIDIInputListener::noteOnReceived, message.getChannel(), message.getNoteNumber(), message.getVelocity());
	else if (message.isNoteOff()) inputListeners.call(&MIDIInputListener::noteOffReceived, message.getChannel(), message.getNoteNumber(), 0); //force note off to velocity 0
	else if (message.isController()) inputListeners.call(&MIDIInputListener::controlChangeReceived, message.getChannel(), message.getControllerNumber(), message.getControllerValue());
	else if (message.isProgramChange()) inputListeners.call(&MIDIInputListener::programChangeReceived, message.getChannel(), message.getProgramChangeNumber());
	else if (message.isFullFrame()) inputListeners.call(&MIDIInputListener::fullFrameTimecodeReceived, message);
	else if (message.isQuarterFrame()) inputListeners.call(&MIDIInputListener::quarterFrameTimecodeReceived, message);
	else if (message.isPitchWheel()) inputListeners.call(&MIDIInputListener::pitchWheelReceived, message.getChannel(), message.getPitchWheelValue());
	else if (message.isChannelPressure()) inputListeners.call(&MIDIInputListener::channelPressureReceived, message.getChannel(), message.getChannelPressureValue());
	else if (message.isAftertouch()) inputListeners.call(&MIDIInputListener::afterTouchReceived, message.getChannel(), message.getNoteNumber(), message.getAfterTouchValue());
	else if (message.isMidiClock()) inputListeners.call(&MIDIInputListener::midiClockReceived);
	else if (message.isMidiStart()) inputListeners.call(&MIDIInputListener::midiStartReceived);
	else if (message.isMidiStop()) inputListeners.call(&MIDIInputListener::midiStopReceived);
	else if (message.isMidiContinue()) inputListeners.call(&MIDIInputListener::midiContinueReceived);
	else if (message.isMidiMachineControlMessage()) inputListeners.call(&MIDIInputListener::midiMachineControlCommandReceived, message.getMidiMachineControlCommand());
	else if (message.isSysEx()) inputListeners.call(&MIDIInputListener::sysExReceived, message);
	else
	{
		int hours, minutes, seconds, frames;
		if (message.isMidiMachineControlGoto(hours, minutes, seconds, frames)) inputListeners.call(&MIDIInputListener::midiMachineControlGotoReceived, hours, minutes, seconds, frames);
		else
		{
			DBG("Not handled : " << message.getDescription());
		}
	}
}






//*****************   MIDI OUTPUT


MIDIOutputDevice::MIDIOutputDevice(const MidiDeviceInfo& info) :
	MIDIDevice(info, MIDI_OUT),
	Thread("MIDI Out " + info.name),
	usageCount(0)
{}

MIDIOutputDevice::~MIDIOutputDevice()
{
	//MIDIManager deletes a device that was unplugged, whatever its users still think
	stopThread(1000);
	const ScopedLock sl(queueLock);
	queue.clear();
	device.reset();
}

void MIDIOutputDevice::open()
{
	usageCount++;
	if (usageCount != 1) return;

	std::unique_ptr<MidiOutput> d = MidiOutput::openDevice(id);
	if (d != nullptr) LOG("MIDI Out " << d->getName() << " opened");
	else LOGERROR("MIDI Out " << name << " open error");

	{
		const ScopedLock sl(queueLock);
		queue.clear();
		device = std::move(d);
	}

	if (device != nullptr) startThread(Thread::Priority::high);
}

void MIDIOutputDevice::close()
{
	if (usageCount <= 0) return;
	usageCount--;
	if (usageCount != 0) return;

	//let what is queued go out (bounded : a stuck driver must not hang the caller), then no more
	signalThreadShouldExit();
	notify();
	if (!waitForThreadToExit(2000)) stopThread(500);

	std::unique_ptr<MidiOutput> d;
	{
		const ScopedLock sl(queueLock);
		queue.clear();
		d = std::move(device);
	}
	d.reset();
	LOG("MIDI Out " << name << " closed");
}

bool MIDIOutputDevice::isOpen()
{
	const ScopedLock sl(queueLock);
	return device != nullptr;
}

void MIDIOutputDevice::sendNoteOn(int channel, int pitch, int velocity)
{
	sendMessageNow(MidiMessage::noteOn(channel, pitch, (uint8)velocity));
}

void MIDIOutputDevice::sendNoteOff(int channel, int pitch)
{
	sendMessageNow(MidiMessage::noteOff(channel, pitch));
}

void MIDIOutputDevice::sendControlChange(int channel, int number, int value)
{
	sendMessageNow(MidiMessage::controllerEvent(channel, number, value));
}

void MIDIOutputDevice::sendProgramChange(int channel, int number)
{
	sendMessageNow(MidiMessage::programChange(channel, number));
}

void MIDIOutputDevice::sendSysEx(Array<uint8> data)
{
	sendMessageNow(MidiMessage::createSysExMessage(data.getRawDataPointer(), data.size()));
}

void MIDIOutputDevice::sendFullframeTimecode(int hours, int minutes, int seconds, int frames, MidiMessage::SmpteTimecodeType timecodeType)
{
	sendMessageNow(MidiMessage::fullFrame(hours, minutes, seconds, frames, timecodeType));
}

void MIDIOutputDevice::sendQuarterframe(int piece, int value)
{
	sendMessageNow(MidiMessage::quarterFrame(piece, value));
}

void MIDIOutputDevice::sendMidiMachineControlCommand(MidiMessage::MidiMachineControlCommand command)
{
	sendMessageNow(MidiMessage::midiMachineControlCommand(command));
}

void MIDIOutputDevice::sendMidiMachineControlGoto(int hours, int minutes, int seconds, int frames)
{
	sendMessageNow(MidiMessage::midiMachineControlGoto(hours, minutes, seconds, frames));
}

void MIDIOutputDevice::sendPitchWheel(int channel, int value)
{
	sendMessageNow(MidiMessage::pitchWheel(channel, value));
}

void MIDIOutputDevice::sendChannelPressure(int channel, int value)
{
	sendMessageNow(MidiMessage::channelPressureChange(channel, value));
}

void MIDIOutputDevice::sendAfterTouch(int channel, int note, int value)
{
	sendMessageNow(MidiMessage::aftertouchChange(channel, note, value));
}

void MIDIOutputDevice::sendMessageNow(const MidiMessage& message)
{
	{
		const ScopedLock sl(queueLock);
		if (device == nullptr) return;
		if ((int)queue.size() >= maxQueueSize)
		{
			droppedCount++;
			return;
		}
		queue.push_back(message);
	}
	notify();
}

void MIDIOutputDevice::run()
{
	while (true)
	{
		MidiMessage m;
		bool have = false;
		int dropped = 0;
		{
			const ScopedLock sl(queueLock);
			if (!queue.empty())
			{
				m = queue.front();
				queue.pop_front();
				have = true;
			}
			dropped = droppedCount;
			droppedCount = 0;
		}

		if (dropped > 0) LOGWARNING("MIDI Out " << name << " : dropped " << dropped << " message(s), the device is not keeping up");

		if (have)
		{
			//only this thread sends, and close() stops it before replacing the device
			if (device != nullptr) device->sendMessageNow(m);
			continue;
		}

		if (threadShouldExit()) break; //close() : exit once the queue is empty
		wait(100);
	}
}
