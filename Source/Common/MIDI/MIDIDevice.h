/*
  ==============================================================================

    MIDIDevice.h
    Created: 20 Dec 2016 1:17:56pm
    Author:  Ben

  ==============================================================================
*/

#pragma once

class MIDIDevice
{
public:
	enum Type { MIDI_IN, MIDI_OUT };
	MIDIDevice(const MidiDeviceInfo &deviceName, Type t);
	virtual ~MIDIDevice() {}

	String id;
	String name;
	Type type;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MIDIDevice)
};

class MIDIInputDevice :
	public MIDIDevice,
	public MidiInputCallback
{
public:
	MIDIInputDevice(const MidiDeviceInfo &info);
	~MIDIInputDevice();
	std::unique_ptr<MidiInput> device;

	// Inherited via MidiInputCallback
	virtual void handleIncomingMidiMessage(MidiInput * source, const MidiMessage & message) override;

	class  MIDIInputListener
	{
	public:
		/** Destructor. */
		virtual ~MIDIInputListener() {}
		virtual void noteOnReceived(const int &/*channel*/, const int &/*pitch*/, const int &/*velocity*/) {}
		virtual void noteOffReceived(const int &/*channel*/, const int &/*pitch*/, const int &/*velocity*/) {}
		virtual void controlChangeReceived(const int &/*channel*/, const int &/*number*/, const int &/*value*/) {}
    virtual void programChangeReceived(const int &/*channel*/, const int &/*value*/) {}
		virtual void sysExReceived(const MidiMessage &/*msg*/) {}
		virtual void fullFrameTimecodeReceived(const MidiMessage&/*msg*/) {}
		virtual void quarterFrameTimecodeReceived(const MidiMessage&/*msg*/) {}
		virtual void pitchWheelReceived(const int&/*channel*/, const int&/*value*/) {}
		virtual void channelPressureReceived(const int&/*channel*/, const int&/*value*/) {}
		virtual void afterTouchReceived(const int &/*channel*/, const int & /*note*/, const int &/*value*/){}
		virtual void midiMessageReceived(const MidiMessage& message) {}
		virtual void midiClockReceived() {}
		virtual void midiStartReceived() {}
		virtual void midiStopReceived() {}
		virtual void midiContinueReceived() {}
		virtual void midiMachineControlCommandReceived(const MidiMessage::MidiMachineControlCommand &/*type*/) {}
		virtual void midiMachineControlGotoReceived(const int&/*hours*/, const int&/*minutes*/, const int&/*seconds*/, const int&/*frames*/) {}
	};

	//Listeners are called on the MIDI driver's thread, holding this lock. Adding and removing take it too, so a listener
	//is never called again once its removal returns - the next thing its owner does may be to delete it.
	CriticalSection listenerLock;
	ListenerList<MIDIInputListener> inputListeners;
	void addMIDIInputListener(MIDIInputListener* newListener);
	void removeMIDIInputListener(MIDIInputListener* listener);

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MIDIInputDevice)

};


//Sends are queued and go out in order on this device's own thread : the callers (a sequence's play thread, a clock,
//the MTC sender, the message thread) never wait on the driver, which busy-waits for a whole SysEx (165 ms for 512 bytes),
//and none of them ever touches the MidiOutput, which close() replaces.
class MIDIOutputDevice :
	public MIDIDevice,
	private Thread
{
public:
	MIDIOutputDevice(const MidiDeviceInfo &info);
	~MIDIOutputDevice();

	int usageCount; //open / close, message thread

	void open();
	void close(); //when the last user closes, what is still queued goes out first (a final note off, a clock stop)
	bool isOpen();

	void sendNoteOn(int channel, int pitch, int velocity);
	void sendNoteOff(int channel, int pitch);
	void sendControlChange(int channel, int number, int value);
	void sendProgramChange(int channel, int number);
	void sendSysEx(Array<uint8> data);
	void sendFullframeTimecode(int hours, int minutes, int seconds, int frames, MidiMessage::SmpteTimecodeType timecodeType);
	void sendQuarterframe(int piece, int value);
	void sendMidiMachineControlCommand(MidiMessage::MidiMachineControlCommand command);
	void sendMidiMachineControlGoto(int hours, int minutes, int seconds, int frames);
	void sendPitchWheel(int channel, int value);
	void sendChannelPressure(int channel, int value);
	void sendAfterTouch(int channel, int note, int value);
	void sendMessageNow(const MidiMessage &message); //queued, see above

private:
	CriticalSection queueLock; //the device and the queue
	std::unique_ptr<MidiOutput> device;
	std::deque<MidiMessage> queue;
	static constexpr int maxQueueSize = 4096; //a device that stopped draining must not grow memory without bound
	int droppedCount = 0;

	void run() override;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MIDIOutputDevice)
};
