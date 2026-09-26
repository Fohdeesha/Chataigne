/*
  ==============================================================================

	DMXDevice.cpp
	Created: 7 Apr 2017 11:22:47am
	Author:  Ben

  ==============================================================================
*/

#include "JuceHeader.h"

DMXDevice::DMXDevice(const String& name, Type _type, bool canReceive) :
	ControllableContainer(name),
	type(_type),
	enabled(true),
	isConnected(nullptr),
	canReceive(canReceive),
	sendExtraWaitMS(0),
	inputCC(nullptr),
	outputCC(nullptr),
	senderThread(this)
{
	saveAndLoadRecursiveData = true;

	DMXManager::getInstance()->addDMXManagerListener(this);


	if (canReceive)
	{
		inputCC = new EnablingControllableContainer("Input");
		inputCC->enabled->setValue(false);
		addChildControllableContainer(inputCC, true);
	}

	outputCC = new EnablingControllableContainer("Output");
	sendRate = outputCC->addIntParameter("Send Rate", "The rate at which to send data.", 44, 1, 200);
	sendRate->canBeDisabledByUser = true;

	addChildControllableContainer(outputCC, true);

	updateSenderThread();
}

DMXDevice::~DMXDevice()
{
	senderThread.stopThread(1000);
	if (DMXManager::getInstanceWithoutCreating() != nullptr) DMXManager::getInstance()->removeDMXManagerListener(this);
}


void DMXDevice::setEnabled(bool value)
{
	if (enabled == value) return;
	enabled = value;
	refreshEnabled();
	updateSenderThread();
}

void DMXDevice::updateSenderThread()
{
	bool shouldRun = enabled && outputCC->enabled->boolValue() && sendRate->enabled;

	if (shouldRun != senderThread.isThreadRunning())
	{
		if (shouldRun) senderThread.startThread();
		else senderThread.stopThread(1000);
	}
}

void DMXDevice::updateConnectedParam()
{
	if (shouldHaveConnectionParam())
	{
		isConnected = addBoolParameter("Connected", "If checked, the device is connected", false);
		isConnected->isControllableFeedbackOnly = true;
		isConnected->isSavable = false;
		isConnected->hideInEditor = true;
	}
	else
	{
		removeControllable(isConnected);
		isConnected = nullptr;
	}

	dmxDeviceListeners.call(&DMXDeviceListener::dmxDeviceSetupChanged, this);
}

bool DMXDevice::shouldHaveConnectionParam()
{
	return canReceive && inputCC != nullptr && inputCC->enabled->boolValue();
}


void DMXDevice::setDMXValuesIn(int net, int subnet, int universe, Array<uint8> values, const String& sourceName)
{
	jassert(values.size() == DMX_NUM_CHANNELS);
	dmxDeviceListeners.call(&DMXDeviceListener::dmxDataInChanged, this, net, subnet, universe, values, sourceName);
}



void DMXDevice::onControllableStateChanged(Controllable* c)
{
	if (c == sendRate) updateSenderThread();
}

void DMXDevice::onControllableFeedbackUpdate(ControllableContainer* cc, Controllable* c)
{
	if (inputCC != nullptr && c == inputCC->enabled) updateConnectedParam();
	else if (c == outputCC->enabled) updateSenderThread();
}

int DMXDevice::getFirstUniverse()
{
	return 0;
}

void DMXDevice::setDMXValues(DMXUniverse* u, int numChannels)
{
	//a consistent copy : writers change the values from other threads, a multi-channel write under the same lock
	Array<uint8> frame = u->takeFrame(false);
	setDMXValues(u->net, u->subnet, u->universe, frame.getRawDataPointer(), jmin(numChannels, frame.size()));
}

void DMXDevice::setDMXValues(int net, int subnet, int universe, uint8* values, int numChannels)
{
	if (!outputCC->enabled->boolValue()) return;

	{
		GenericScopedLock lock(universesToSend.getLock());
		DMXUniverse* targetU = nullptr;
		for (auto& u : universesToSend)
		{
			if (u->net == net && u->subnet == subnet && u->universe == universe)
			{
				targetU = u;
				break;
			}
		}

		if (targetU == nullptr)
		{
			targetU = new DMXUniverse(net, subnet, universe);
			universesToSend.add(targetU);
		}

		targetU->updateValues(Array<uint8>(values, numChannels));
	}

	if (!sendRate->enabled)
	{
		sendDMXValues();
	}
}

bool DMXDevice::sendDMXValues()
{
	if (!enabled) return false;

	//Take the list under its lock and send outside it : clearing it after the lock was released threw away whatever
	//was set in between, and that frame never went out
	OwnedArray<DMXUniverse, juce::CriticalSection> toSend;
	{
		GenericScopedLock lock(universesToSend.getLock());
		toSend.swapWith(universesToSend);
	}

	if (toSend.isEmpty()) return false;

	const ScopedLock sl(sendLock);
	for (auto& u : toSend) sendDMXValuesInternal(u->net, u->subnet, u->universe, u->values.getRawDataPointer(), DMX_NUM_CHANNELS);
	return true;
}

void DMXDevice::frameReady()
{
	if (senderThread.isThreadRunning()) senderThread.notify();
}


void DMXDevice::clearDevice()
{
	senderThread.stopThread(1000);
}

DMXDevice* DMXDevice::create(Type type)
{
	switch (type)
	{
	case OPENDMX:
		return new DMXOpenUSBDevice();
		break;

	case ENTTEC_DMXPRO:
		return new DMXEnttecProDevice();
		break;

	case ENTTEC_MK2:
		return new DMXEnttecProDevice(); //tmp but seems to work for 1 universe
		break;

	case ARTNET:
		return new DMXArtNetDevice();
		break;

	case SACN:
		return new DMXSACNDevice();
		break;

	default:
		DBG("Not handled");
		break;
	}

	return nullptr;
}

DMXDevice::SenderThread::SenderThread(DMXDevice* d) :
	Thread("DMX Sender Thread"),
	device(d)
{
}

DMXDevice::SenderThread::~SenderThread() {
	stopThread(1000);
}

void DMXDevice::SenderThread::run() {
	//The module fills a whole frame on its own clock and calls frameReady() : this thread then sends it at once, so there
	//is one clock. It used to send on its own 40 Hz timer too, beating against the module's : whatever the module had
	//filled waited up to one more frame, and a frame filled twice between two sends was lost. The device's send rate
	//stays the maximum rate (and, with sendExtraWaitMS, the spacing a serial interface needs). The timeout is only a
	//safety net for values set without a frameReady() : waking on it mid-frame would send half of a multi-universe frame.
	double lastSend = 0;
	while (!threadShouldExit())
	{
		const double rateGapMs = 1000.0 / jmax(1, device->sendRate->intValue());
		const double gapMs = rateGapMs + device->sendExtraWaitMS;
		wait((int)std::ceil(gapMs * 4));
		if (threadShouldExit()) break;

		//a notify must not raise the rate, but a frame of the module's own clock arriving a hair early (both at 40 Hz,
		//with a millisecond of jitter) must not wait a millisecond either : a little slack on the rate part only,
		//never on the spacing a serial interface needs
		const double slackMs = jmin(2.0, rateGapMs * 0.1);
		const double since = Time::getMillisecondCounterHiRes() - lastSend;
		if (since < gapMs - slackMs) Thread::sleep((int)std::ceil(gapMs - slackMs - since));

		//only a real send counts : a wake that found nothing must not hold back the frame that arrives right after it
		if (device->sendDMXValues()) lastSend = Time::getMillisecondCounterHiRes();
	}
}
