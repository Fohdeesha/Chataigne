/*
  ==============================================================================

	OpenHABModule.h
	Created: 23 Sep 2026

	Every openHAB item as a typed value : discovered over REST, kept current by
	openHAB's server-sent events, and written back as item commands.

	Threads : EventThread does discovery and holds the event stream, CommandThread
	sends commands over a keep-alive connection. Neither touches the model : what
	they learn goes through the inbox to the message thread (handleAsyncUpdate),
	which owns every parameter.

  ==============================================================================
*/

#pragma once

class OpenHABModule :
	public Module,
	public EngineListener,
	public juce::AsyncUpdater,
	public juce::Timer
{
public:
	OpenHABModule(const String& name = "openHAB");
	virtual ~OpenHABModule();

	StringParameter* host;
	IntParameter* port;
	StringParameter* apiToken;
	EnablingControllableContainer authenticationCC;
	StringParameter* username;
	StringParameter* password;
	BoolParameter* isConnected;
	StringParameter* serverStatus;

	Trigger* syncItems;
	BoolParameter* followChanges;
	StringParameter* itemFilter;
	BoolParameter* includeReadOnly;
	Trigger* removeMissing;

	BoolParameter* sendCommands;
	FloatParameter* maxCommandRate;
	IntParameter* echoWindow;

	struct ItemInfo
	{
		String name;
		OpenHAB::ItemSpec spec;
		OpenHAB::Kind kind = OpenHAB::Kind::NONE;
		WeakReference<Parameter> param;
		bool missing = false;
		bool rangeFromServer = false;

		//hue and saturation of the last lit colour, sent or reported, so black or grey keeps them
		double hue = 0;
		double saturation = 0;
		bool hasHSB = false;
		String unit;

		uint32 lastLocalChange = 0;
		uint32 lastRemoteWhilePending = 0;
		bool reconcilePending = false; //a command went out, openHAB's answer is checked once it has been quiet for the echo window
		String believed; //what openHAB is thought to hold, in command form
		String remoteType;
		String remoteValue;
		bool hasRemote = false;
		uint32 lastWarning = 0;
	};

	OwnedArray<ItemInfo> items;
	HashMap<String, ItemInfo*> itemMap;
	Array<ItemInfo*> reconcileList;

	//Adding values one by one makes an open Inspector rebuild every value each time and notifies the whole
	//engine : thousands of items took minutes with the module selected. A batch folds the values and marks them
	//loading, hides what it creates, and rebuilds once at the end if anything was added, removed or reordered.
	class ValuesBatch
	{
	public:
		ValuesBatch(OpenHABModule& m);
		~ValuesBatch();
		OpenHABModule& module;
		bool wasCollapsed;
		bool wasLoading;
		bool nested;
	};
	ValuesBatch* activeBatch = nullptr;
	Array<WeakReference<Parameter>> batchCreated;
	bool batchStructureChanged = false;

	bool applyingStructure; //building values from the item list : nothing is sent
	Array<Parameter*> applyingRemoteParams; //values being set from openHAB's state : only these are not sent back
	bool startRequested;
	String serverVersion;
	int skippedCount;

	struct Config
	{
		String host;
		int port = 8080;
		String headers;
		String authHint; //what a 401 means for the credentials this config sends
	};

	struct Inbound
	{
		enum Type { SERVER_INFO, STRUCTURE, STATES, STATE, CONNECTED, DISCONNECTED };
		Type type = STATE;
		int generation = 0;
		String name;
		String stateType;
		String value;
		var data;
	};

	struct PendingCommand
	{
		String item;
		String command;
		uint32 queuedAt = 0;
		bool coalesce = true;
		int generation = 0;
	};

	CriticalSection inboxLock;
	Array<Inbound> inbox;
	std::atomic<int> generation;

	CriticalSection queueLock;
	Array<PendingCommand> commandQueue;
	WaitableEvent commandWake;
	std::atomic<double> minCommandIntervalMs;
	std::atomic<bool> resyncRequested;
	std::atomic<bool> followRegistryChanges;

	class EventThread;
	class CommandThread;
	std::unique_ptr<EventThread> eventThread;
	std::unique_ptr<CommandThread> commandThread;
	OwnedArray<Thread> retiredThreads; //told to stop on a restart, deleted once they have

	void clearItem() override;

	void onContainerParameterChangedInternal(Parameter* p) override;
	void onControllableFeedbackUpdateInternal(ControllableContainer* cc, Controllable* c) override;

	void afterLoadJSONDataInternal() override;
	void fileLoaded() override;

	void startConnection();
	void stopConnection(bool waitForThreads);
	void restartConnection();
	void reapRetiredThreads(bool wait);
	Config buildConfig();

	void post(const Inbound& msg); //any thread
	void handleAsyncUpdate() override;
	void timerCallback() override;

	void applyStructure(const var& list);
	void applyStates(const var& list);
	void handleRemoteState(ItemInfo& info, const String& stateType, const String& value);
	void applyRemote(ItemInfo& info);
	void applyStateToParameter(ItemInfo& info, const String& stateType, const String& value);

	ItemInfo* getOrCreateInfo(const String& name);
	void ensureParameter(ItemInfo& info);
	Parameter* createParameter(ItemInfo& info);
	void configureParameter(ItemInfo& info, Parameter* p);
	void updateEnumOptions(ItemInfo& info, EnumParameter* ep);
	void markMissing(ItemInfo& info);
	void removeItem(const String& name);
	void removeMissingItems();
	String describeItem(const ItemInfo& info) const;
	static String parameterTypeFor(OpenHAB::Kind kind);
	static OpenHAB::Kind kindFromCustomData(const var& customData);

	void valueChanged(Parameter* p);
	String commandFromParameter(ItemInfo& info);
	bool sameCommand(const ItemInfo& info, const String& a, const String& b, bool asEcho) const;
	void queueCommand(ItemInfo& info, const String& command);
	void queueRawCommand(const String& itemName, const String& command); //any thread
	bool hasQueuedCommand(const String& itemName);
	bool takeNextCommand(int generation, PendingCommand& out, int& waitMs, HashMap<String, uint32>& lastSent, int& expired); //command thread
	void requeueCommand(const PendingCommand& cmd); //command thread

	bool warnOnce(ItemInfo& info);
	void setStatus(const String& s);

	static var sendCommandFromScript(const var::NativeFunctionArgs& args);

	static OpenHABModule* create() { return new OpenHABModule(); }
	virtual String getDefaultTypeString() const override { return "openHAB"; }
};
