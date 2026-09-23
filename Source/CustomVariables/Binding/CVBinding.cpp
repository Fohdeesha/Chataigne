/*
  ==============================================================================

	CVBinding.cpp
	Created: 19 Sep 2026

  ==============================================================================
*/

// ---------------------------------------------------------------------------
// helpers

static bool cvVarIsNumeric(const var& v)
{
	return v.isDouble() || v.isInt() || v.isInt64() || v.isBool();
}

// Cross-binding recursion guard.
//
// A single binding is protected by its own isSending flag, and a two-binding cycle (A's command
// writes B's variable, B's command writes A's) normally terminates on Parameter::setValue's
// equality short-circuit. That short-circuit is defeated by "Always Notify changes", which any
// user can switch on from a parameter's right-click menu - and then the cycle never ends.
// Everything here runs on the message thread, so one counter is enough.
static int cvSendDepth = 0;
static const int CV_MAX_SEND_DEPTH = 8;

static bool cvVarEquals(const var& a, const var& b)
{
	if (a.isArray() || b.isArray())
	{
		if (!a.isArray() || !b.isArray()) return false;
		if (a.size() != b.size()) return false;
		for (int i = 0; i < a.size(); ++i) if (!cvVarEquals(a[i], b[i])) return false;
		return true;
	}

	if (cvVarIsNumeric(a) && cvVarIsNumeric(b)) return std::abs((double)a - (double)b) < 1e-9;
	return a.toString() == b.toString();
}


// ---------------------------------------------------------------------------

CVBinding::CVBinding(CVVariable* owner) :
	ControllableContainer("Binding"),
	owner(owner),
	valueMap("Value Map"),
	outValuesCC("Out Value"),
	outValue(nullptr),
	applyingFeedback(false),
	isLoadingBinding(false),
	initialSyncDone(false),
	echoPending(false),
	echoSentAtMS(0),
	hasLastSent(false),
	lastSendMS(0),
	sendQueued(false),
	sendTimer(this),
	reconcileTimer(this),
	startupTimer(this),
	isSending(false)
{
	editorCanBeCollapsed = true;
	editorIsCollapsed = true;

	bindingEnabled = addBoolParameter("Binding Enabled", "When off, this variable behaves exactly as an unbound Custom Variable : nothing is sent and feedback is ignored.", false);

	feedbackFrom = addTargetParameter("Feedback From", "A controllable - typically a module value - whose changes are mirrored INTO this variable. Feedback never causes anything to be sent.", nullptr);
	feedbackFrom->excludeTypesFilter.add(Trigger::getTypeStringStatic());

	addChildControllableContainer(&valueMap);

	echoWindow = addIntParameter("Echo Window", "After sending, ignore incoming feedback that DISAGREES with what was just sent, for this long.\n\
This is what stops a control snapping back while a device takes time to act. When the window closes without the device confirming, the feedback source is read again and the device wins, so a long window only delays a correction, it never loses one.\n\
1000 ms covers OSC and MQTT round trips and an HTTP poll at 1 Hz. 0 disables it.", 1000, 0, 60000);

	deadband = addFloatParameter("Deadband", "Ignore changes smaller than this, in either direction. Stops a rounded round trip oscillating on continuous values.\n\
Left at 0 because the useful size depends entirely on the variable's range - try about 0.5% of it (0.005 for a 0-1 control, 0.5 for 0-100).", 0, 0, 1000);

	maxSendRate = addFloatParameter("Max Send Rate", "Maximum outgoing commands per second; when changes arrive faster they are coalesced and the newest value wins.\n\
50 Hz matches the rate the preset interpolation thread runs at, so preset recalls pass through untouched while a sequence or a flood of OSC cannot overrun the target.\n\
Lower it to about 20 for HTTP, which serialises one request per round trip. 0 is unlimited.", 50, 0, 500);

	onLoad = addEnumParameter("On Load", "What to do once the project has finished loading.\n\
Adopt from feedback : take the feedback source's current value and send nothing, so opening a project never switches a device.\n\
Push to device : send the value saved in the project.\n\
Do nothing : leave both sides as they are.");
	//the first option is the default. It was "Push to device" until 2026-09-23 : a value left at its default is not
	//written to a project, so an enabled binding always saves it (updateEnabledState), or its file would push on an
	//older build and adopt on this one
	onLoad->addOption("Adopt from feedback", OL_ADOPT)->addOption("Push to device", OL_PUSH)->addOption("Do nothing", OL_NOTHING);

	startupDelay = addFloatParameter("Startup Delay", "How long to wait after the project loads before On Load acts, so modules have a chance to connect.", 2.f, 0, 60);

	outValuesCC.hideInEditor = true;
	outValuesCC.hideInRemoteControl = true; //derived, not worth a line in every project's OSC tree
	addChildControllableContainer(&outValuesCC);

	sendTo.reset(new MappingOutputManager(nullptr));
	sendTo->setNiceName("Send To");
	addChildControllableContainer(sendTo.get());

	sendToWhenFalse.reset(new MappingOutputManager(nullptr));
	sendToWhenFalse->setNiceName("Send To When False");
	sendToWhenFalse->editorIsCollapsed = true;
	addChildControllableContainer(sendToWhenFalse.get());

	sendNow = addTrigger("Send Now", "Send the current value immediately, ignoring deadband and rate limit.");
	pullFromFeedback = addTrigger("Pull From Feedback", "Read the feedback source now and adopt its value, without sending anything.");

	//nothing may reach a device until the initial sync runs
	sendTo->setForceDisabled(true);
	sendToWhenFalse->setForceDisabled(true);

	rebuildOutValue();
	updateEnabledState();

	if (Engine::mainEngine != nullptr)
	{
		Engine::mainEngine->addEngineListener(this);

		//created live by the user rather than loaded from a file : no load event is coming
		if (!Engine::mainEngine->isLoadingFile)
		{
			WeakReference<CVBinding> safeThis(this);
			MessageManager::callAsync([safeThis]()
				{
					if (CVBinding* b = safeThis.get()) b->doInitialSync();
				});
		}
	}
}

CVBinding::~CVBinding()
{
	sendTimer.stopTimer();
	reconcileTimer.stopTimer();
	startupTimer.stopTimer();

	if (Engine::mainEngine != nullptr) Engine::mainEngine->removeEngineListener(this);

	setFeedbackParam(nullptr);

	masterReference.clear();
}


// --- accessors ---------------------------------------------------------------

Parameter* CVBinding::getControlParameter() const
{
	if (owner == nullptr) return nullptr;
	return dynamic_cast<Parameter*>(owner->controllable);
}

Controllable::Type CVBinding::getControlType() const
{
	if (owner == nullptr || owner->controllable == nullptr) return Controllable::CUSTOM;
	return owner->controllable->type;
}

bool CVBinding::isActive() const
{
	if (owner == nullptr || owner->controllable == nullptr) return false;
	if (bindingEnabled == nullptr || !bindingEnabled->boolValue()) return false;
	if (isLoadingBinding || isCurrentlyLoadingData) return false;
	//never send while the engine is loading or tearing down : a startup timer or a queued
	//rate-limited send could otherwise fire a command at a half-destroyed module
	if (Engine::mainEngine != nullptr && (Engine::mainEngine->isLoadingFile || Engine::mainEngine->isClearing)) return false;
	return true;
}


// --- feedback wiring ---------------------------------------------------------

void CVBinding::setFeedbackParam(Parameter* p)
{
	if (feedbackParam != nullptr && !feedbackParam.wasObjectDeleted())
	{
		feedbackParam->removeParameterListener(this);
	}

	feedbackParam = p;

	if (feedbackParam != nullptr && !feedbackParam.wasObjectDeleted())
	{
		feedbackParam->addParameterListener(this);
	}
}

void CVBinding::updateFeedbackFromTarget()
{
	setFeedbackParam(feedbackFrom != nullptr ? dynamic_cast<Parameter*>(feedbackFrom->target.get()) : nullptr);
}

void CVBinding::rebuildOutValue()
{
	const String wanted = valueMap.getOutputControllableType(getControlType());

	if (outValue != nullptr && outValue->getTypeString() == wanted)
	{
		sendTo->setOutParams({ outValue }, 0);
		sendToWhenFalse->setOutParams({ outValue }, 0);
		return;
	}

	if (outValue != nullptr)
	{
		outValuesCC.removeControllable(outValue);
		outValue = nullptr;
	}

	if (Controllable* c = ControllableFactory::createControllable(wanted))
	{
		outValue = dynamic_cast<Parameter*>(c);
		if (outValue != nullptr)
		{
			outValue->setNiceName("Value");
			outValue->isSavable = false;
			outValue->setControllableFeedbackOnly(true);
			outValuesCC.addParameter(outValue);
		}
		else
		{
			delete c;
		}
	}

	//never hand a null into setOutParams : getMergedOutValue dereferences entries whose
	//WeakReference was built from nullptr (wasObjectDeleted() is false for those)
	Array<Parameter*> outs;
	if (outValue != nullptr) outs.add(outValue);
	sendTo->setOutParams(outs, 0);
	sendToWhenFalse->setOutParams(outs, 0);
}

void CVBinding::updateEnabledState()
{
	const bool on = bindingEnabled != nullptr && bindingEnabled->boolValue();

	feedbackFrom->hideInEditor = !on;
	valueMap.hideInEditor = !on;
	echoWindow->hideInEditor = !on;
	deadband->hideInEditor = !on;
	maxSendRate->hideInEditor = !on;
	onLoad->hideInEditor = !on;
	onLoad->forceSaveValue = on;
	startupDelay->hideInEditor = !on;
	sendTo->hideInEditor = !on;
	//only a true/false variable ever uses it (performSend) : a number or text variable sends everything through
	//Send To, so the list would be a box that does nothing - unless something was already put in it
	sendToWhenFalse->hideInEditor = !on || (getControlType() != Controllable::BOOL && sendToWhenFalse->items.size() == 0);
	sendNow->hideInEditor = !on;
	pullFromFeedback->hideInEditor = !on;

	queuedNotifier.addMessage(new ContainerAsyncEvent(ContainerAsyncEvent::ControllableContainerNeedsRebuild, this));
}


// --- outbound ----------------------------------------------------------------

void CVBinding::controlValueChanged()
{
	if (applyingFeedback) return;
	if (!isActive()) return;

	Parameter* p = getControlParameter();
	if (p == nullptr) return;

	var v = p->getValue();
	if (EnumParameter* ep = dynamic_cast<EnumParameter*>(p)) v = ep->getValueKey();

	requestSend(v, false);
}

void CVBinding::controlTriggered()
{
	if (applyingFeedback) return;
	if (!isActive()) return;

	requestSend(var(true), true);
}

void CVBinding::requestSend(const var& controlValue, bool bypassDeadband)
{
	if (!isActive()) return;

	//deadband on the control side
	if (!bypassDeadband && deadband->floatValue() > 0 && hasLastSent
		&& cvVarIsNumeric(controlValue) && cvVarIsNumeric(lastSentControlValue))
	{
		if (std::abs((double)controlValue - (double)lastSentControlValue) < deadband->floatValue()) return;
	}

	const float rate = maxSendRate->floatValue();
	if (rate <= 0)
	{
		performSend(controlValue);
		return;
	}

	const uint32 now = Time::getMillisecondCounter();
	const uint32 minInterval = (uint32)jmax(1.f, 1000.f / rate);

	if (!hasLastSent || (now - lastSendMS) >= minInterval)
	{
		performSend(controlValue);
		return;
	}

	//coalesce : keep only the newest value, fire when the window opens
	queuedControlValue = controlValue;
	sendQueued = true;
	sendTimer.startTimer((int)(minInterval - (now - lastSendMS)));
}

void CVBinding::SendTimer::timerCallback()
{
	stopTimer();
	if (binding == nullptr) return;
	if (!binding->sendQueued) return;

	binding->sendQueued = false;
	binding->performSend(binding->queuedControlValue);
}

void CVBinding::performSend(const var& controlValue)
{
	if (!isActive()) return;
	if (sendTo == nullptr) return;

	//a Send To command that writes back to this same variable would recurse; Parameter::setValue's
	//equality short-circuit usually stops it, but not for an alwaysNotify parameter
	if (isSending) return;
	const ScopedValueSetter<bool> sending(isSending, true);

	//...and the same cycle across two bindings is not covered by that flag
	if (cvSendDepth >= CV_MAX_SEND_DEPTH)
	{
		NLOGWARNING(niceName, "Binding send depth limit reached (" << CV_MAX_SEND_DEPTH << "). A cycle between bindings was stopped - check that two bindings are not writing each other's variables.");
		return;
	}
	const ScopedValueSetter<int> depth(cvSendDepth, cvSendDepth + 1);

	bool ok = true;
	const var mapped = valueMap.forward(controlValue, getControlType(), &ok);
	if (!ok)
	{
		NLOGWARNING(niceName, "No Value Map entry for control value \"" << controlValue.toString() << "\", nothing sent.");
		return;
	}

	if (outValue == nullptr) rebuildOutValue();
	if (outValue == nullptr) return;

	//pick the list : a bool with a populated false-list uses it when false
	MappingOutputManager* m = sendTo.get();
	if (getControlType() == Controllable::BOOL && sendToWhenFalse->items.size() > 0 && !(bool)controlValue)
	{
		m = sendToWhenFalse.get();
	}

	if (m->items.size() == 0) return; //nothing wired : not an error

	outValue->setValue(mapped);
	m->updateOutputValues(0, false);

	echoValue = mapped;
	echoPending = echoWindow->intValue() > 0;
	echoSentAtMS = Time::getMillisecondCounter();

	lastSentControlValue = controlValue;
	hasLastSent = true;
	lastSendMS = echoSentAtMS;

	//close the loop even if the device never says anything
	if (echoPending) reconcileTimer.startTimer(echoWindow->intValue() + 1);
}

void CVBinding::ReconcileTimer::timerCallback()
{
	stopTimer();
	if (binding != nullptr) binding->reconcileWithFeedback();
}

void CVBinding::reconcileWithFeedback()
{
	if (!echoPending) return; //device already confirmed
	echoPending = false;

	if (!isActive()) return;
	if (feedbackParam == nullptr || feedbackParam.wasObjectDeleted()) return;

	//the feedback source holds the device's latest word : if it still disagrees, it wins
	handleFeedbackValue(feedbackParam->getValue());
}


// --- inbound -----------------------------------------------------------------

void CVBinding::handleFeedbackValue(const var& deviceValue)
{
	if (!isActive()) return;

	Parameter* p = getControlParameter();
	if (p == nullptr) return;

	const uint32 now = Time::getMillisecondCounter();

	if (echoPending)
	{
		if (cvVarEquals(deviceValue, echoValue))
		{
			//the device confirmed what we sent : nothing to correct
			echoPending = false;
		}
		else if ((now - echoSentAtMS) < (uint32)echoWindow->intValue())
		{
			//stale pre-command state on its way back : ignore it rather than snap the control back
			return;
		}
		else
		{
			//window expired : the device genuinely disagrees, let it win
			echoPending = false;
		}
	}

	bool ok = true;
	const var mapped = valueMap.inverse(deviceValue, getControlType(), &ok);
	if (!ok) return; //device value not in the map : ignore rather than guess

	//deadband on the inbound side
	if (deadband->floatValue() > 0 && cvVarIsNumeric(mapped) && cvVarIsNumeric(p->getValue()))
	{
		if (std::abs((double)mapped - (double)p->getValue()) < deadband->floatValue()) return;
	}

	applyingFeedback = true;

	if (EnumParameter* ep = dynamic_cast<EnumParameter*>(p)) ep->setValueWithKey(mapped.toString());
	else p->setValue(mapped);

	applyingFeedback = false;
}

void CVBinding::doInitialSync()
{
	if (owner == nullptr || owner->controllable == nullptr) return;

	sendTo->setForceDisabled(false);
	sendToWhenFalse->setForceDisabled(false);
	initialSyncDone = true;

	if (!bindingEnabled->boolValue()) return;

	//Re-resolve unconditionally. TargetParameter resolves its own target from an EngineListener
	//callback too, and listener order between it and this binding is not guaranteed - so the
	//resolve done in endLoadFile() can land before the target exists.
	updateFeedbackFromTarget();

	switch (onLoad->getValueDataAsEnum<OnLoadMode>())
	{
	case OL_PUSH:
	{
		if (Parameter* p = getControlParameter())
		{
			var v = p->getValue();
			if (EnumParameter* ep = dynamic_cast<EnumParameter*>(p)) v = ep->getValueKey();
			requestSend(v, true);
		}
	}
	break;

	case OL_ADOPT:
	{
		if (feedbackParam != nullptr && !feedbackParam.wasObjectDeleted()) handleFeedbackValue(feedbackParam->getValue());
	}
	break;

	case OL_NOTHING:
	default:
		break;
	}
}


// --- container plumbing ------------------------------------------------------

void CVBinding::onContainerParameterChanged(Parameter* p)
{
	ControllableContainer::onContainerParameterChanged(p);

	if (p == feedbackFrom) updateFeedbackFromTarget();
	else if (p == bindingEnabled) updateEnabledState();
}

void CVBinding::onControllableFeedbackUpdate(ControllableContainer* cc, Controllable* c)
{
	ControllableContainer::onControllableFeedbackUpdate(cc, c);

	//the map's Mode / Output Type decide what type the outgoing value is carried by, so the
	//parameter feeding the commands has to follow them, not just be built once at load
	if (cc == &valueMap && (c == valueMap.outputType || c == valueMap.mode)) rebuildOutValue();
}

void CVBinding::onExternalParameterValueChanged(Parameter* p)
{
	ControllableContainer::onExternalParameterValueChanged(p);

	if (p != nullptr && p == feedbackParam) handleFeedbackValue(p->getValue());
}

void CVBinding::onContainerTriggerTriggered(Trigger* t)
{
	ControllableContainer::onContainerTriggerTriggered(t);

	if (t == sendNow)
	{
		if (Parameter* p = getControlParameter())
		{
			var v = p->getValue();
			if (EnumParameter* ep = dynamic_cast<EnumParameter*>(p)) v = ep->getValueKey();
			requestSend(v, true);
		}
	}
	else if (t == pullFromFeedback)
	{
		updateFeedbackFromTarget();
		if (feedbackParam != nullptr && !feedbackParam.wasObjectDeleted())
		{
			echoPending = false; //explicit user request : do not suppress
			handleFeedbackValue(feedbackParam->getValue());
		}
	}
}

void CVBinding::inspectableDestroyed(Inspectable* i)
{
	if (feedbackParam != nullptr && i == feedbackParam.get()) setFeedbackParam(nullptr);
}

void CVBinding::endLoadFile()
{
	//On Load acts once per binding. A load clears the whole session before building the new one, so a binding that
	//has already synced and still hears this is in a session the load left alone : a document that cannot be read
	//ends that way (organicui returns before clear() and still ends the load), and acting again would re-send a Push
	if (initialSyncDone) return;

	updateFeedbackFromTarget();
	rebuildOutValue();

	//restarting rather than scheduling : the second of the two startup loads wins, and On Load
	//acts exactly once
	startupTimer.startTimer(jmax(1, (int)(startupDelay->floatValue() * 1000)));
}

void CVBinding::StartupTimer::timerCallback()
{
	stopTimer();
	if (binding != nullptr) binding->doInitialSync();
}


// --- persistence -------------------------------------------------------------

var CVBinding::getJSONData(bool includeNonOverriden)
{
	var data = ControllableContainer::getJSONData(includeNonOverriden);

	data.getDynamicObject()->setProperty("valueMap", valueMap.getJSONData(includeNonOverriden));
	data.getDynamicObject()->setProperty("sendTo", sendTo->getJSONData(includeNonOverriden));

	var fData = sendToWhenFalse->getJSONData(includeNonOverriden);
	if (!fData.isVoid() && fData.getDynamicObject() != nullptr && fData.getDynamicObject()->getProperties().size() > 0)
		data.getDynamicObject()->setProperty("sendToWhenFalse", fData);

	return data;
}

void CVBinding::loadJSONDataInternal(var data)
{
	isLoadingBinding = true;

	ControllableContainer::loadJSONDataInternal(data);

	if (data.getDynamicObject() != nullptr)
	{
		if (data.getDynamicObject()->hasProperty("valueMap")) valueMap.loadJSONData(data.getProperty("valueMap", var()));
		if (data.getDynamicObject()->hasProperty("sendTo")) sendTo->loadJSONData(data.getProperty("sendTo", var()));
		if (data.getDynamicObject()->hasProperty("sendToWhenFalse")) sendToWhenFalse->loadJSONData(data.getProperty("sendToWhenFalse", var()));
	}

	isLoadingBinding = false;

	rebuildOutValue();
	updateEnabledState();
}

InspectableEditor* CVBinding::getEditorInternal(bool isRoot, Array<Inspectable*> inspectables)
{
	//buildAtCreation must stay true (the default). With it false the constructor passes
	//doNotRebuild to setCollapsed(), so an editor created for a section that is NOT collapsed shows
	//the open arrow and lays out its header, but never builds its children : the Binding section
	//came up expanded and empty, and only filled in after collapsing and re-expanding it by hand.
	return new GenericControllableContainerEditor(this, isRoot);
}
