/*
  ==============================================================================

	MQTTModule.h
	Created: 10 Apr 2022 3:11:05pm
	Author:  bkupe

  ==============================================================================
*/

#pragma once

#if JUCE_WINDOWS || JUCE_LINUX || ( JUCE_MAC && (defined(__arm64__) || defined(__aarch64__)))
#define MOSQUITTO_SUPPORTED
#endif

#ifdef MOSQUITTO_SUPPORTED
#include <mosquittopp.h>
#endif

class MQTTTopic :
	public BaseItem
{
public:
	enum Protocol { RAW, JSON, DEFAULT };

	MQTTTopic(var params = var());
	virtual ~MQTTTopic();

	StringParameter* topic;
	EnumParameter* protocol;
	int mid;

	//InspectableEditor* getEditorInternal(bool isRoot, Array<Inspectable*> inspectables = Array<Inspectable*>()) override;

	DECLARE_TYPE("Topic");
};

class MQTTClientModule :
	public Module
#ifdef MOSQUITTO_SUPPORTED
	, public mosqpp::mosquittopp
#endif
	, public Thread
	, public BaseManager<MQTTTopic>::ManagerListener
{
public:
	MQTTClientModule(const String& name = "MQTT Client", bool canHaveInput = true, bool canHaveOutput = true);
	virtual ~MQTTClientModule();


	EnumParameter* protocol;

	StringParameter* clientId;
	StringParameter* host;
	IntParameter* port;


	IntParameter* keepAlive;
	BoolParameter* isConnected;
	Trigger* clearValues = nullptr; //never created : compared against every changed controllable, it was uninitialised


	EnablingControllableContainer authenticationCC;
	StringParameter* username;
	StringParameter* pass;
	//BoolParameter* useTLS;
	HashMap<String, MQTTTopic*> topicItemMap;

	CriticalSection updateTopicLock;
	CriticalSection mosquittoLock;
	BaseManager<MQTTTopic> topicsManager;
	std::atomic<uint32> lastPublishWarningTime { 0 };

	//What clientId resolved to for the current connection. Written in run() before connecting and
	//read by the mosquitto callbacks, which libmosquitto calls from inside loop_forever(), i.e. on
	//this same thread.
	String resolvedClientId;



	const Identifier dataEventId = "dataEvent";

	void clearItem() override;

	void onContainerParameterChangedInternal(Parameter* p) override;
	void onControllableFeedbackUpdateInternal(ControllableContainer* cc, Controllable* c) override;

	void publishMessage(const String& topic, const String& message);
	void handleMessage(const juce::String& topic, const juce::String& data); //message thread only

	void itemAdded(MQTTTopic* item) override;
	void itemsAdded(Array<MQTTTopic*> item) override;
	void itemRemoved(MQTTTopic* item) override;
	void itemsRemoved(Array<MQTTTopic*> item) override;

	void updateTopicSubs();

	//New modules resolve per machine, so one project can run on several machines against one broker.
	//Modules saved before this default existed keep the old one (see loadJSONDataItemInternal).
	static constexpr const char* DEFAULT_CLIENT_ID = "chataigne-{machine}";
	static constexpr const char* LEGACY_DEFAULT_CLIENT_ID = "Chataigne";

	void loadJSONDataItemInternal(var data) override;
	void afterLoadJSONDataInternal() override;

	void run() override;

	void stopClient(); //stops and joins the thread : only where the module goes away
	void stopClientNoWait();

	//A setting changed : the thread may be inside connect(), which blocks until the broker answers or the system gives
	//up (21 s for an address that never answers), and waiting for it froze the interface that long. The thread is told
	//to stop, and started again once it has left.
	void restartClient();
	void checkRestart();
	class RestartTimer :
		public Timer
	{
	public:
		RestartTimer(MQTTClientModule& m) : module(m) {}
		MQTTClientModule& module;
		void timerCallback() override { module.checkRestart(); }
	};
	RestartTimer restartTimer{ *this };
	uint32 restartAskedAt = 0;
	bool restartLogged = false;

	bool shouldLogPublishWarning();
	String resolveClientId() const;

	//Script
	static var publishMessageFromScript(const var::NativeFunctionArgs& args);

	//mosquitto
#ifdef MOSQUITTO_SUPPORTED
	void on_connect(int rc) override;
	void resubscribeAll(); //message thread only
	virtual void on_connect_with_flags(int /*rc*/, int /*flags*/) override { return; }
	virtual void on_disconnect(int rc) override;
	virtual void on_publish(int mid) override;
	virtual void on_message(const struct mosquitto_message* message) override;
	virtual void on_subscribe(int mid, int qos_count, const int* granted_qos) override;
	virtual void on_unsubscribe(int mid) override;
	virtual void on_log(int level, const char* str) override;
	virtual void on_error() override;
#endif

	static MQTTClientModule* create() { return new MQTTClientModule(); }
	virtual String getDefaultTypeString() const override { return "MQTT Client"; }
};
