#include "JuceHeader.h"
#include "TimeMachineIncludes.h"
#include "ChataigneSequenceManager.h"
/*
  ==============================================================================

    SequenceManager.cpp
    Created: 28 Oct 2016 8:13:04pm
    Author:  bkupe

  ==============================================================================
*/

juce_ImplementSingleton(ChataigneSequenceManager)

ControllableContainer* getAppSettings();

ChataigneSequenceManager::ChataigneSequenceManager() :
	SequenceManager()
{
	module.reset(new SequenceModule(this));

	itemDataType = "Sequence";
	helpID = "TimeMachine";

	snapKeysToFrames = getAppSettings()->addBoolParameter("Snap Keys to Frames", "If checked, all mapping keys in sequences, will be automatically snapped to frames", false);
	captureOverwriteDistance = getAppSettings()->addFloatParameter("Capture Overwrite Distance", "When adding a key from the recorder's input, if an existing key is within this time distance (in seconds) of the playhead, its value is overwritten instead of adding a new key next to it. Half a frame is used as a minimum, so 0 keeps exact-position overwrite only.", 0.1f, 0, 1);

	OSCRemoteControl::getInstance()->addRemoteControlListener(this);
}

ChataigneSequenceManager::~ChataigneSequenceManager()
{
	if (OSCRemoteControl* rc = OSCRemoteControl::getInstanceWithoutCreating()) rc->removeRemoteControlListener(this);
}

Sequence* ChataigneSequenceManager::createItem()
{
	return new ChataigneSequence();
}

Sequence* ChataigneSequenceManager::getCurrentEditingSequence()
{
	if (ShapeShifterManager* sm = ShapeShifterManager::getInstanceWithoutCreating())
	{
		if (TimeMachineView* view = sm->getContentForType<TimeMachineView>())
		{
			if (view->editor != nullptr) return view->editor->sequence;
		}
	}

	return nullptr;
}

void ChataigneSequenceManager::processMessage(const OSCMessage& m, const String& clientId)
{
	//Stable OSC addresses targeting the sequence currently opened in the editor, independent of sequence and layer names :
	// /sequences/current/layers/<layer number, 1-based>/<controllable> or /sequences/current/<controllable or container path>
	//OSC Remote Control only calls this for addresses that didn't match an existing controllable, from its network thread : resolve and handle on the message thread

	StringArray addrSplit;
	addrSplit.addTokens(m.getAddressPattern().toString(), "/", "\"");
	addrSplit.removeEmptyStrings();

	if (addrSplit.size() < 3 || addrSplit[0] != shortName || addrSplit[1] != "current") return;

	OSCMessage msg(m);
	MessageManager::callAsync([msg]() { if (ChataigneSequenceManager* sm = getInstanceWithoutCreating()) sm->handleCurrentSequenceMessage(msg); });
}

void ChataigneSequenceManager::handleCurrentSequenceMessage(const OSCMessage& m)
{
	String addr = m.getAddressPattern().toString();

	Sequence* s = getCurrentEditingSequence();
	if (s == nullptr)
	{
		NLOGWARNING(niceName, "Received " << addr << " but no sequence is currently opened in the editor");
		return;
	}

	StringArray addrSplit;
	addrSplit.addTokens(addr, "/", "\"");
	addrSplit.removeEmptyStrings();
	addrSplit.removeRange(0, 2); //sequences/current

	ControllableContainer* container = s;
	if (addrSplit.size() >= 2 && addrSplit[0] == s->layerManager->shortName && addrSplit[1].containsOnly("0123456789"))
	{
		int layerIndex = addrSplit[1].getIntValue() - 1; //1-based, ordered top to bottom as in the editor
		if (layerIndex < 0 || layerIndex >= s->layerManager->items.size())
		{
			NLOGWARNING(niceName, "Sequence " << s->niceName << " has no layer #" << addrSplit[1] << ", it has " << s->layerManager->items.size() << " layers");
			return;
		}

		container = s->layerManager->items[layerIndex];
		addrSplit.removeRange(0, 2);
	}

	if (addrSplit.isEmpty())
	{
		NLOGWARNING(niceName, "Address " << addr << " targets a container, not a controllable");
		return;
	}

	Controllable* c = container->getControllableForAddress(addrSplit);
	if (c == nullptr)
	{
		NLOGWARNING(niceName, "No controllable found for " << addr << " in sequence " << s->niceName);
		return;
	}

	OSCHelpers::handleControllableForOSCMessage(c, m);
}

void ChataigneSequenceManager::createSequenceFromAudioFile(File f)
{
	if (ModuleManager::getInstance()->getItemsWithType<AudioModule>().size() == 0) {
		AudioModule* m = new AudioModule();
		ModuleManager::getInstance()->addItem(m);
	}

	ChataigneSequence* seq = new ChataigneSequence();
	addItem(seq);
	seq->setNiceName(f.getFileNameWithoutExtension());

	ChataigneAudioLayer* l = new ChataigneAudioLayer(seq, var());
	seq->layerManager->addItem(l);
	l->uiHeight->setValue(80);

	AudioLayerClip* clip = new AudioLayerClip();
	l->clipManager.addItem(clip);
	clip->filePath->setValue(f.getFullPathName());
}

void ChataigneSequenceManager::showMenuAndGetSequenceStatic(ControllableContainer* startFromCC, std::function<void(Sequence*)> returnFunc)
{
	getInstance()->showMenuAndGetSequence(startFromCC, returnFunc);
}

void ChataigneSequenceManager::showMenuAndGetLayerStatic(ControllableContainer* startFromCC, std::function<void(SequenceLayer*)> returnFunc)
{
	getInstance()->showMenuAndGetLayer(startFromCC, returnFunc);
}

void ChataigneSequenceManager::showMenuAndGetCueStatic(ControllableContainer* startFromCC, std::function<void(TimeCue*)> returnFunc)
{
	getInstance()->showMenuAndGetCue(startFromCC, returnFunc);
}

void ChataigneSequenceManager::showMenuAndGetAudioLayerStatic(ControllableContainer* startFromCC, std::function<void(AudioLayer*)> returnFunc)
{
	getInstance()->showMenuAndGetAudioLayer(startFromCC, returnFunc);
}

void ChataigneSequenceManager::showMenuAndGetTriggerStatic(ControllableContainer* startFromCC, std::function<void(TimeTrigger*)> returnFunc)
{
	getInstance()->showMenuAndGetTrigger(startFromCC, returnFunc);
}
