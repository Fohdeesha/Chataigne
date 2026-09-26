/*
  ==============================================================================

	OSCRemoteControl.h
	Created: 23 Apr 2018 5:00:30pm
	Author:  Ben

  ==============================================================================
*/

#pragma once


#if ORGANICUI_USE_SERVUS
#include "servus/servus.h"
#endif

#if ORGANICUI_USE_WEBSERVER
#include <juce_simpleweb/juce_simpleweb.h>
#include "OSCPacketHelper.h"
#endif

class OSCRemoteControl :
	public EnablingControllableContainer,
#if ORGANICUI_USE_SERVUS
	public juce::Thread,
#endif

#if ORGANICUI_USE_WEBSERVER
	public SimpleWebSocketServer::Listener,
	public SimpleWebSocketServer::RequestHandler,
	public CustomLogger::LoggerListener,
	public WarningReporter::AsyncListener,
#endif
	//Received on the network thread, handled on the message thread : handling a message walks and changes the object
	//tree and fires triggers whose listeners do real work, none of which may run while the message thread edits, loads or
	//deletes. The network thread only fills a bounded inbox (see enqueue), which the message thread drains.
	public juce::OSCReceiver::Listener<juce::OSCReceiver::RealtimeCallback>,
	private juce::AsyncUpdater
{
public:
	juce_DeclareSingleton(OSCRemoteControl, true);

	OSCRemoteControl();
	~OSCRemoteControl();

	IntParameter* localPort;
	juce::OSCReceiver receiver;
	bool receiverIsConnected;
	BoolParameter* logIncoming;
	BoolParameter* logOutgoing;
	BoolParameter* sendFeedbackOnListen;
	BoolParameter* enableSendLogFeedback;

	EnablingControllableContainer manualSendCC;
	juce::OSCSender manualSender;
	StringParameter* manualAddress;
	IntParameter* manualPort;

	void setupReceiver();
	void setupManualSender();

#if ORGANICUI_USE_SERVUS
	servus::Servus servus;

#if ORGANICUI_USE_WEBSERVER
	servus::Servus oscQueryServus;
#endif

	void setupZeroconf();
#endif

	void updateEngineListener();

	void processMessage(const juce::OSCMessage& m, const juce::String& sourceId = "");

	void onContainerParameterChanged(Parameter* p) override;

	void oscMessageReceived(const juce::OSCMessage& m) override;
	void oscBundleReceived(const juce::OSCBundle& b) override;

	//A flood faster than the message thread can handle must not grow memory and latency without bound : past this many
	//waiting messages the newest are dropped, as a full UDP buffer would drop them
	static constexpr int maxInboxSize = 20000;
	juce::CriticalSection inboxLock;
	struct Received
	{
		juce::OSCMessage message;
		juce::String sourceId; //the websocket client, empty for UDP
	};
	std::deque<Received> inbox;
	int droppedMessages = 0;
	juce::uint32 lastDropWarning = 0;
	void enqueue(const juce::OSCMessage& m, const juce::String& sourceId = juce::String()); //network or websocket thread
	void handleAsyncUpdate() override; //message thread

#if ORGANICUI_USE_SERVUS
	void run() override;
#endif

#if ORGANICUI_USE_WEBSERVER
	std::unique_ptr<SimpleWebSocketServer> server;

	//Changed on the message thread only, read by whatever thread fires feedback (a trigger notifies on its caller's
	//thread) : iterate them under their lock, since a HashMap iterator does not take it
	juce::HashMap<juce::String, juce::Array<Controllable*>, juce::DefaultHashFunctions, juce::CriticalSection> feedbackMap;
	juce::HashMap<Controllable*, juce::String, juce::DefaultHashFunctions, juce::CriticalSection> noFeedbackMap;

	std::function<void(juce::var& metaData)> fillHostInfoMetaDataFunc;

	void setupServer();
	bool handleHTTPRequest(std::shared_ptr<HttpServer::Response> response, std::shared_ptr<HttpServer::Request> request) override;
	juce::var buildHTTPData(const juce::String& path, const juce::String& query, bool& downloadMode, juce::String& downloadFileName); //message thread

	void serverInitSuccess() override;
	void serverInitError(const juce::String& message) override;
	void connectionOpened(const juce::String& id) override;
	void messageReceived(const juce::String& id, const juce::String& message) override;
	void dataReceived(const juce::String& id, const juce::MemoryBlock& data) override;
	void connectionClosed(const juce::String& id, int status, const juce::String& reason) override;
	void connectionError(const juce::String& id, int status, const juce::String& message) override;

	//the websocket callbacks above run on the server's io thread and hand their work to these, on the message thread
	void handleConnectionOpened(const juce::String& id);
	void handleConnectionClosed(const juce::String& id, int status, const juce::String& reason, bool isError);
	void handleCommand(const juce::String& id, const juce::var& o);
	void handleValues(const juce::var& o);


	void sendOSCQueryFeedback(Controllable* c, const juce::String& excludeId = "");
	void sendOSCQueryStateFeedback(Controllable* c, const juce::String& excludeId = "");
	void sendOSCQueryFeedback(const juce::OSCMessage& m, juce::StringArray excludes = juce::StringArray());
	void sendOSCQueryFeedbackTo(const juce::OSCMessage& m, juce::StringArray ids = juce::StringArray());

	void sendPathAddedFeedback(const juce::String& path);
	void sendPathRemovedFeedback(const juce::String& path);
	void sendPathNameChangedFeedback(const juce::String& oldPath, const juce::String& newPath);
	void sendPathChangedFeedback(const juce::String& path);

	bool hasClient(const juce::String& id);

	void controllableFeedbackUpdate(ControllableContainer* cc, Controllable* c) override;
	void controllableStateUpdate(ControllableContainer* cc, Controllable* c) override;

	void addControllableToNoFeedbackMap(Controllable* c, const juce::String& id, const juce::String& fallbackId);

	//void newMessage(const ContainerAsyncEvent& e) override;
	void onControllableFeedbackUpdate(ControllableContainer* cc, Controllable* c) override;

	void newMessage(const CustomLogger::LogEvent& e) override;
	void sendLogFeedback(const juce::String& type, const juce::String& source, const juce::String& message);
	void sendPersistentWarningFeedback(const juce::String& address, const juce::String& warningID, const juce::String& warningMessage);
	void newMessage(const WarningReporter::WarningReporterEvent& e) override;

#endif


	void sendAllManualFeedback();
	void sendManualFeedbackForControllable(Controllable* c);


	class RemoteControlListener
	{
	public:
		virtual ~RemoteControlListener() {}
		virtual void processMessage(const juce::OSCMessage& m, const juce::String& clientId) {}
		virtual void clientConnected(const juce::String& clientId) {}
		virtual void clientDisconnected(const juce::String& clientId, const juce::String& message = "") {}
	};

	DECLARE_INSPECTACLE_CRITICAL_LISTENER(RemoteControl, remoteControl)

};

