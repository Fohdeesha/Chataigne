/*
  ==============================================================================

	CVBinding.h
	Created: 19 Sep 2026

	A two-way binding attached to a Custom Variable.

	  Feedback From : a controllable (typically a module value) whose changes are
	                  mirrored INTO the variable, never sent back out.
	  Send To       : commands fired when the variable changes, carrying the
	                  mapped value exactly as a Mapping output does.
	  Value Map     : an invertible transform between the two sides.

	This replaces the "one Mapping out, one Mapping back, plus a hand-set
	sendOnInputChangeOnly" arrangement with a single object whose loop safety is a
	property of the object rather than of how carefully it was assembled.

	NOTE ON INCLUDES: BaseCommandHandler.h pulls in Module/ModuleIncludes.h, which
	pulls in CustomVariablesModule.h, which pulls in CustomVariablesIncludes.h.
	Including it from this header would therefore be circular, so
	MappingOutputManager is forward-declared and only used from the .cpp.

  ==============================================================================
*/

#pragma once

class MappingOutputManager;
class CVVariable;

class CVBinding :
	public ControllableContainer,
	public Inspectable::InspectableListener,
	public EngineListener
{
public:
	CVBinding(CVVariable* owner);
	~CVBinding();

	CVVariable* owner;

	enum OnLoadMode { OL_PUSH, OL_ADOPT, OL_NOTHING };

	//config
	BoolParameter* bindingEnabled;
	TargetParameter* feedbackFrom;
	CVValueMap valueMap;

	IntParameter* echoWindow;		//ms : ignore feedback that disagrees with what we just sent, for this long
	FloatParameter* deadband;		//ignore changes smaller than this, both ways (0 = off)
	FloatParameter* maxSendRate;	//Hz, 0 = unlimited. Coalesces to the newest value.
	EnumParameter* onLoad;
	FloatParameter* startupDelay;	//s : how long after project load before On Load acts

	ControllableContainer outValuesCC;
	Parameter* outValue;			//carries the mapped value into the Send To commands

	std::unique_ptr<MappingOutputManager> sendTo;
	std::unique_ptr<MappingOutputManager> sendToWhenFalse;

	Trigger* sendNow;
	Trigger* pullFromFeedback;

	//live state
	WeakReference<Parameter> feedbackParam;

	bool applyingFeedback;		//true while writing the control from feedback : suppresses outbound
	bool handlingFeedback;		//re-entry guard : a feedback source that follows the control itself would recurse
	bool isLoadingBinding;
	bool initialSyncDone;		//On Load has acted : a later endLoadFile() is not a load of this binding

	bool echoPending;
	var echoValue;				//the mapped value we last sent
	uint32 echoSentAtMS;

	bool hasLastSent;
	var lastSentControlValue;

	uint32 lastSendMS;
	bool sendQueued;
	var queuedControlValue;

	//accessors on the owning variable
	Parameter* getControlParameter() const;
	Controllable::Type getControlType() const;

	void setFeedbackParam(Parameter* p);
	bool feedbackFollowsControl(Parameter* p) const;
	static var readFeedbackValue(Parameter* p);
	void updateFeedbackFromTarget();
	void rebuildOutValue();
	void updateEnabledState();

	//called by CVVariable
	void controlValueChanged();
	void controlTriggered();

	//engine
	void handleFeedbackValue(const var& deviceValue);
	void requestSend(const var& controlValue, bool bypassDeadband);
	void sendNowInternal();
	void pullFromFeedbackInternal();
	void performSend(const var& controlValue);
	void doInitialSync();

	bool isActive() const;

	void onContainerParameterChanged(Parameter* p) override;
	void onExternalParameterValueChanged(Parameter* p) override;
	void onControllableFeedbackUpdate(ControllableContainer* cc, Controllable* c) override;
	void onContainerTriggerTriggered(Trigger* t) override;
	void inspectableDestroyed(Inspectable* i) override;

	//EngineListener
	void endLoadFile() override;

	var getJSONData(bool includeNonOverriden = false) override;
	void loadJSONDataInternal(var data) override;

	InspectableEditor* getEditorInternal(bool isRoot, Array<Inspectable*> inspectables = Array<Inspectable*>()) override;

	//rate-limit flush
	class SendTimer : public Timer
	{
	public:
		SendTimer(CVBinding* b) : binding(b) {}
		CVBinding* binding;
		void timerCallback() override;
	};
	SendTimer sendTimer;

	//When the echo window closes with the device still unconfirmed, re-read the feedback source
	//and reconcile. Without this the window can swallow the device's real state permanently:
	//a module value only notifies on CHANGE, so a device that reported its true state during the
	//window - or that never changes at all - would leave the control showing a lie.
	class ReconcileTimer : public Timer
	{
	public:
		ReconcileTimer(CVBinding* b) : binding(b) {}
		CVBinding* binding;
		void timerCallback() override;
	};
	ReconcileTimer reconcileTimer;

	void reconcileWithFeedback();

	//Chataigne loads a command-line project TWICE at startup, so endLoadFile() arrives twice and a
	//fire-and-forget callAfterDelay would push On Load twice. A restartable member timer collapses
	//them to one push, and to the later (fully loaded) one.
	class StartupTimer : public Timer
	{
	public:
		StartupTimer(CVBinding* b) : binding(b) {}
		CVBinding* binding;
		void timerCallback() override;
	};
	StartupTimer startupTimer;

	//guards a Send To command that writes back to its own variable
	bool isSending;

	WeakReference<CVBinding>::Master masterReference;
	friend class WeakReference<CVBinding>;
};
