/*
  ==============================================================================

	OpenHABModule.cpp
	Created: 23 Sep 2026

  ==============================================================================
*/

#include "Module/ModuleIncludes.h"

namespace OpenHABModuleConstants
{
	const char* eventTopics = "openhab/items/*/stateupdated,openhab/items/*/statechanged,openhab/items/*/added,openhab/items/*/removed,openhab/items/*/updated";
	const int commandExpiryMs = 5000;
	const int maxQueuedCommands = 10000;
}

//==============================================================================
// Event thread : discovery, the event stream and state snapshots. Posts what it learns, never touches the model.

class OpenHABModule::EventThread :
	public Thread
{
public:
	EventThread(OpenHABModule& m, const Config& c, int gen) :
		Thread("openHAB events"),
		module(m),
		config(c),
		generation(gen),
		rest([this] { return threadShouldExit(); }),
		stream([this] { return threadShouldExit(); })
	{
	}

	~EventThread() override
	{
		stopThread(10000);
	}

	OpenHABModule& module;
	Config config;
	int generation;
	OpenHAB::HttpConnection rest;
	OpenHAB::HttpConnection stream;
	bool useUpdatedEvents = true;

	void post(Inbound m)
	{
		m.generation = generation;
		module.post(m);
	}

	void run() override
	{
		int backoffMs = 1000;

		while (!threadShouldExit())
		{
			String failure;
			const bool streamed = session(failure);
			if (threadShouldExit()) break;

			if (streamed) backoffMs = 1000;

			Inbound m;
			m.type = Inbound::DISCONNECTED;
			m.value = failure;
			m.stateType = String(backoffMs / 1000.0, 0, false);
			post(m);

			//a notify() that came while streaming (Sync Items) is still signalled and would end the wait at once
			wait(0);
			wait(backoffMs);
			backoffMs = jmin(backoffMs * 2, 30000);
		}

		rest.close();
		stream.close();
	}

	static String describe(const String& what, const OpenHAB::Response& r)
	{
		if (r.status == 401 || r.status == 403) return what + " : " + r.describe() + ", the server wants an API token";
		return what + " : " + r.describe();
	}

	bool fetchStructure(String& failure)
	{
		OpenHAB::Response r = rest.request("GET", "/rest/items?staticDataOnly=true", {}, {}, 60000);
		if (!r.succeeded())
		{
			failure = describe("reading the item list", r);
			return false;
		}

		var list = JSON::parse(r.getBodyAsString());
		if (!list.isArray())
		{
			failure = "the item list is not a JSON array";
			return false;
		}

		Inbound m;
		m.type = Inbound::STRUCTURE;
		m.data = list;
		post(m);
		return true;
	}

	bool fetchStates(String& failure)
	{
		OpenHAB::Response r = rest.request("GET", "/rest/items?fields=name,state", {}, {}, 60000);
		if (!r.succeeded())
		{
			failure = describe("reading item states", r);
			return false;
		}

		var list = JSON::parse(r.getBodyAsString());
		if (!list.isArray())
		{
			failure = "the state list is not a JSON array";
			return false;
		}

		Inbound m;
		m.type = Inbound::STATES;
		m.data = list;
		post(m);
		return true;
	}

	//returns true once the event stream was up, so the caller knows to reset its backoff
	bool session(String& failure)
	{
		rest.setTarget(config.host, config.port);
		rest.setExtraHeaders(config.headers);
		stream.setTarget(config.host, config.port);
		stream.setExtraHeaders(config.headers);

		OpenHAB::Response r = rest.request("GET", "/rest/", {}, {}, 5000);
		if (!r.succeeded())
		{
			failure = describe("contacting " + config.host + ":" + String(config.port), r);
			return false;
		}

		var info = JSON::parse(r.getBodyAsString());
		if (!info.isObject() || !info.hasProperty("version"))
		{
			failure = config.host + ":" + String(config.port) + " did not answer like openHAB's REST API";
			return false;
		}

		const String version = info.getProperty("runtimeInfo", var()).getProperty("version", "").toString();
		useUpdatedEvents = version.isEmpty() || version.getIntValue() >= 4;

		Inbound si;
		si.type = Inbound::SERVER_INFO;
		si.value = version;
		post(si);

		module.resyncRequested = false; //about to read everything anyway
		if (!fetchStructure(failure)) return false;

		//subscribe before taking the state snapshot, so a change in between is not lost
		OpenHAB::Response head;
		if (!stream.openStream("/rest/events?topics=" + String(OpenHABModuleConstants::eventTopics), "text/event-stream", 5000, head) || head.status != 200)
		{
			failure = describe("opening the event stream", head);
			stream.close();
			return false;
		}

		if (!fetchStates(failure))
		{
			stream.close();
			return false;
		}

		Inbound c;
		c.type = Inbound::CONNECTED;
		c.value = version;
		post(c);

		OpenHAB::SseParser parser;
		uint32 lastData = Time::getMillisecondCounter();
		uint32 structureDirtyAt = 0;
		bool sawUpdatedEvent = false;
		MemoryBlock block;

		failure = "stopped";

		while (!threadShouldExit())
		{
			block.reset();
			int n = 0;
			{
				MemoryOutputStream out(block, false);
				n = stream.readStream(out, 100);
			}

			const uint32 now = Time::getMillisecondCounter();

			if (n < 0)
			{
				failure = "the event stream was closed";
				break;
			}

			if (n > 0)
			{
				lastData = now;
				Array<OpenHAB::SseParser::Event> events;
				const bool ok = parser.feed(static_cast<const char*>(block.getData()), block.getSize(), events);
				handleEvents(events, structureDirtyAt, sawUpdatedEvent);
				if (!ok)
				{
					failure = "an event was larger than " + String((int)(parser.maxEventBytes / (1024 * 1024))) + " MB";
					break;
				}
			}

			//openHAB sends an alive event every 10 s, so silence means the connection is dead
			if ((int)(now - lastData) > 30000)
			{
				failure = "no data from the event stream for 30 s";
				break;
			}

			const bool resync = module.resyncRequested.exchange(false);
			if (resync || (structureDirtyAt != 0 && (int)(now - structureDirtyAt) > 500))
			{
				structureDirtyAt = 0;
				if (!fetchStructure(failure) || !fetchStates(failure)) break;
			}
		}

		stream.close();
		return true;
	}

	void handleEvents(const Array<OpenHAB::SseParser::Event>& events, uint32& structureDirtyAt, bool& sawUpdatedEvent)
	{
		for (auto& e : events)
		{
			if (e.type != "message") continue;

			var d = JSON::parse(e.data);
			if (!d.isObject()) continue;

			OpenHAB::TopicParts t;
			if (!OpenHAB::parseItemTopic(d.getProperty("topic", "").toString(), t)) continue;

			if (t.action == "stateupdated" || t.action == "statechanged")
			{
				//openHAB 4+ sends stateupdated for every update and statechanged on top of it for a change.
				//Older servers only send statechanged.
				if (t.action == "stateupdated") sawUpdatedEvent = true;
				else if (useUpdatedEvents || sawUpdatedEvent) continue;

				var p = JSON::parse(d.getProperty("payload", "").toString());
				if (!p.isObject()) continue;

				Inbound m;
				m.type = Inbound::STATE;
				m.name = t.itemName;
				m.stateType = p.getProperty("type", "").toString();
				m.value = p.getProperty("value", "").toString();
				post(m);
			}
			else if (t.action == "added" || t.action == "removed" || t.action == "updated")
			{
				if (module.followRegistryChanges.load()) structureDirtyAt = Time::getMillisecondCounter() | 1;
			}
		}
	}
};

//==============================================================================
// Command thread : one keep-alive connection, commands taken from the module's queue.

class OpenHABModule::CommandThread :
	public Thread
{
public:
	CommandThread(OpenHABModule& m, const Config& c, int gen) :
		Thread("openHAB commands"),
		module(m),
		config(c),
		generation(gen),
		connection([this] { return threadShouldExit(); })
	{
	}

	~CommandThread() override
	{
		stopThread(10000);
	}

	OpenHABModule& module;
	Config config;
	int generation;
	OpenHAB::HttpConnection connection;

	void run() override
	{
		connection.setTarget(config.host, config.port);
		connection.setExtraHeaders(config.headers);

		HashMap<String, uint32> lastSent;
		HashMap<String, uint32> lastRejectionLog;
		int failures = 0;
		uint32 lastFailureLog = 0;
		int unreportedExpired = 0;

		while (!threadShouldExit())
		{
			PendingCommand cmd;
			int waitMs = 250;
			int expired = 0;
			const bool have = module.takeNextCommand(generation, cmd, waitMs, lastSent, expired);

			const uint32 now = Time::getMillisecondCounter();
			unreportedExpired += expired;
			if (unreportedExpired > 0 && (int)(now - lastFailureLog) > 5000)
			{
				NLOGWARNING(module.niceName, "Dropped " << unreportedExpired << " command(s) that could not be delivered within " << OpenHABModuleConstants::commandExpiryMs / 1000 << " s");
				unreportedExpired = 0;
				lastFailureLog = now;
			}

			if (!have)
			{
				module.commandWake.wait(jlimit(1, 250, waitMs));
				continue;
			}

			OpenHAB::Response r = connection.request("POST", "/rest/items/" + cmd.item, cmd.command, "text/plain; charset=UTF-8", 5000);
			lastSent.set(cmd.item, Time::getMillisecondCounter());
			if (threadShouldExit()) break;

			if (r.error.isNotEmpty())
			{
				module.requeueCommand(cmd);
				failures++;
				if ((int)(Time::getMillisecondCounter() - lastFailureLog) > 5000)
				{
					NLOGWARNING(module.niceName, "Could not send \"" << cmd.command << "\" to " << cmd.item << " : " << r.error << ", retrying");
					lastFailureLog = Time::getMillisecondCounter();
				}
				module.commandWake.wait(jmin(2000, 100 << jmin(failures, 5)));
				continue;
			}

			failures = 0;

			if (!r.succeeded())
			{
				const uint32 t = Time::getMillisecondCounter();
				if (!lastRejectionLog.contains(cmd.item) || (int)(t - lastRejectionLog[cmd.item]) > 5000)
				{
					NLOGWARNING(module.niceName, "openHAB rejected \"" << cmd.command << "\" for " << cmd.item << " : " << r.describe());
					lastRejectionLog.set(cmd.item, t);
				}
			}
			else if (module.logOutgoingData->boolValue())
			{
				NLOG(module.niceName, "Sent " << cmd.item << " <- " << cmd.command);
			}
		}

		connection.close();
	}
};

//==============================================================================

OpenHABModule::OpenHABModule(const String& name) :
	Module(name),
	authenticationCC("Basic Authentication"),
	applyingRemote(false),
	startRequested(false),
	skippedCount(0),
	generation(0),
	minCommandIntervalMs(1000.0 / 20),
	resyncRequested(false),
	followRegistryChanges(true)
{
	alwaysShowValues = true;
	includeValuesInSave = true;
	valuesCC.saveAndLoadRecursiveData = true;
	valuesCC.userCanAddControllables = false;

	setupIOConfiguration(true, true);

	host = moduleParams.addStringParameter("Host", "The openHAB server's address, e.g. openhab.local or an IP address. Plain HTTP only.", "127.0.0.1");
	host->autoTrim = true;
	port = moduleParams.addIntParameter("Port", "openHAB's HTTP port", 8080, 1, 65535);
	apiToken = moduleParams.addStringParameter("API Token", "An openHAB API token (profile page in the Main UI, \"Create new API token\"). Sent as a Bearer token. Needed when openHAB does not allow anonymous access.", "");
	apiToken->autoTrim = true;

	username = authenticationCC.addStringParameter("Username", "openHAB user, only used when openHAB's Basic Authentication is enabled and no API token is set", "");
	password = authenticationCC.addStringParameter("Password", "Password for that user", "");
	authenticationCC.enabled->setValue(false);
	moduleParams.addChildControllableContainer(&authenticationCC);

	isConnected = moduleParams.addBoolParameter("Is Connected", "Connected to openHAB and receiving its events", false);
	isConnected->setControllableFeedbackOnly(true);
	isConnected->isSavable = false;
	connectionFeedbackRef = isConnected;

	serverStatus = moduleParams.addStringParameter("Status", "What the module is doing", "Not connected");
	serverStatus->setControllableFeedbackOnly(true);
	serverStatus->isSavable = false;

	syncItems = moduleParams.addTrigger("Sync Items", "Read the item list and all states again");
	followChanges = moduleParams.addBoolParameter("Follow Item Changes", "Update the values when items are added, removed or changed in openHAB", true);
	itemFilter = moduleParams.addStringParameter("Item Filter", "Only these items become values. Comma separated item names with * and ? wildcards, a leading ! excludes. Empty = every item.", "");
	includeReadOnly = moduleParams.addBoolParameter("Include Read-Only Items", "Also show items openHAB marks as read-only (contacts, sensors), as values that cannot be changed", true);
	removeMissing = moduleParams.addTrigger("Remove Missing Items", "Remove the values of items that no longer exist in openHAB. They are kept until then so links to them do not break.");

	sendCommands = moduleParams.addBoolParameter("Send Commands", "When off, the module only follows openHAB and never sends anything", true);
	maxCommandRate = moduleParams.addFloatParameter("Max Commands Per Second", "Per item. Faster changes are merged and the newest value always goes out. Every command costs openHAB a few milliseconds of CPU, so keep this low when many items fade at once.", 20, 1, 200);
	echoWindow = moduleParams.addIntParameter("Echo Window", "After a change is sent, what openHAB reports for that item is held back until it has been quiet this long (ms), then its last state wins if it differs. Stops a value jumping back while a device fades or moves. Make it longer than your slowest fade. 0 disables.", 2000, 0, 60000);

	defManager->add(CommandDefinition::createDef(this, "", "Set Value", &GenericControllableCommand::create, CommandContext::BOTH)->addParam("action", GenericControllableCommand::SET_VALUE)->addParam("root", (int64)&valuesCC));
	defManager->add(CommandDefinition::createDef(this, "", "Go to Value", &GenericControllableCommand::create, CommandContext::BOTH)->addParam("action", GenericControllableCommand::GO_TO_VALUE)->addParam("root", (int64)&valuesCC));
	defManager->add(CommandDefinition::createDef(this, "", "Send Command", &OpenHABCommand::create, CommandContext::BOTH));

	scriptObject.getDynamicObject()->setMethod("sendCommand", OpenHABModule::sendCommandFromScript);

	if (Engine::mainEngine != nullptr)
	{
		Engine::mainEngine->addEngineListener(this);
		if (!Engine::mainEngine->isLoadingFile)
		{
			//created from the menu or pasted : start once any loadJSONData that follows has run
			startRequested = true;
			triggerAsyncUpdate();
		}
	}
}

OpenHABModule::~OpenHABModule()
{
	if (Engine::mainEngine != nullptr) Engine::mainEngine->removeEngineListener(this);
	stopConnection(true);
	cancelPendingUpdate();
	stopTimer();
}

void OpenHABModule::clearItem()
{
	//the threads must be gone before any value they report into is
	stopConnection(true);
	cancelPendingUpdate();
	stopTimer();
	reconcileList.clear();
	Module::clearItem();
}

//==============================================================================
// Connection

OpenHABModule::Config OpenHABModule::buildConfig()
{
	Config c;
	int portOverride = 0;
	bool https = false;
	c.host = OpenHAB::sanitizeHost(host->stringValue(), portOverride, https);
	c.port = portOverride > 0 ? portOverride : port->intValue();
	if (https) NLOGWARNING(niceName, "HTTPS is not supported, connecting to " << c.host << ":" << c.port << " with plain HTTP");

	if (apiToken->stringValue().isNotEmpty()) c.headers = "Authorization: Bearer " + apiToken->stringValue() + "\r\n";
	else if (authenticationCC.enabled->boolValue()) c.headers = "Authorization: Basic " + Base64::toBase64(username->stringValue() + ":" + password->stringValue()) + "\r\n";
	return c;
}

void OpenHABModule::startConnection()
{
	if (eventThread != nullptr) return;

	const Config c = buildConfig();
	if (c.host.isEmpty())
	{
		setStatus("No host set");
		return;
	}

	const int gen = ++generation;
	{
		const ScopedLock sl(inboxLock);
		inbox.clear();
	}

	minCommandIntervalMs = 1000.0 / jmax(1.0f, maxCommandRate->floatValue());
	followRegistryChanges = followChanges->boolValue();
	resyncRequested = false;

	eventThread.reset(new EventThread(*this, c, gen));
	commandThread.reset(new CommandThread(*this, c, gen));
	eventThread->startThread();
	commandThread->startThread();

	setStatus("Connecting to " + c.host + ":" + String(c.port));
}

//A thread can be inside a connect attempt to an unreachable server, which cannot be interrupted, so a settings
//change must not wait for it on the message thread : the old threads are told to stop and reaped later. Their
//generation no longer matches, so nothing they post is used and they take no new command.
void OpenHABModule::stopConnection(bool waitForThreads)
{
	for (Thread* t : { static_cast<Thread*>(eventThread.get()), static_cast<Thread*>(commandThread.get()) })
	{
		if (t == nullptr) continue;
		t->signalThreadShouldExit();
		t->notify();
	}
	commandWake.signal();

	if (eventThread != nullptr) retiredThreads.add(eventThread.release());
	if (commandThread != nullptr) retiredThreads.add(commandThread.release());

	++generation;

	{
		const ScopedLock sl(queueLock);
		commandQueue.clear();
	}

	if (isConnected->boolValue()) isConnected->setValue(false);

	reapRetiredThreads(waitForThreads);
	if (!retiredThreads.isEmpty() && !isTimerRunning()) startTimer(50);
}

void OpenHABModule::reapRetiredThreads(bool wait)
{
	for (int i = retiredThreads.size() - 1; i >= 0; --i)
	{
		Thread* t = retiredThreads[i];
		if (wait) t->stopThread(10000);
		if (!t->isThreadRunning()) retiredThreads.remove(i);
	}
}

void OpenHABModule::restartConnection()
{
	stopConnection(false);
	if (!enabled->boolValue())
	{
		setStatus("Disabled");
		return;
	}
	if (isCurrentlyLoadingData || Engine::mainEngine == nullptr || Engine::mainEngine->isLoadingFile) return; //fileLoaded() starts it
	startConnection();
}

void OpenHABModule::afterLoadJSONDataInternal()
{
	Module::afterLoadJSONDataInternal();
	if (Engine::mainEngine != nullptr && !Engine::mainEngine->isLoadingFile) restartConnection();
}

void OpenHABModule::fileLoaded()
{
	if (eventThread == nullptr) restartConnection();
}

void OpenHABModule::onContainerParameterChangedInternal(Parameter* p)
{
	Module::onContainerParameterChangedInternal(p);
	if (p == enabled) restartConnection();
}

void OpenHABModule::onControllableFeedbackUpdateInternal(ControllableContainer* cc, Controllable* c)
{
	Module::onControllableFeedbackUpdateInternal(cc, c);

	if (cc == &valuesCC)
	{
		if (Parameter* p = dynamic_cast<Parameter*>(c)) valueChanged(p);
		return;
	}

	if (c == host || c == port || c == apiToken || c == authenticationCC.enabled || c == username || c == password)
	{
		if (!isCurrentlyLoadingData) restartConnection();
	}
	else if (c == syncItems || c == itemFilter || c == includeReadOnly)
	{
		resyncRequested = true;
		if (eventThread != nullptr) eventThread->notify(); //cuts a reconnect wait short
	}
	else if (c == followChanges)
	{
		followRegistryChanges = followChanges->boolValue();
	}
	else if (c == removeMissing)
	{
		removeMissingItems();
	}
	else if (c == maxCommandRate)
	{
		minCommandIntervalMs = 1000.0 / jmax(1.0f, maxCommandRate->floatValue());
	}
}

void OpenHABModule::setStatus(const String& s)
{
	if (serverStatus->stringValue() != s) serverStatus->setValue(s);
}

//==============================================================================
// Inbox, message thread

void OpenHABModule::post(const Inbound& msg)
{
	{
		const ScopedLock sl(inboxLock);
		inbox.add(msg);
	}
	triggerAsyncUpdate();
}

void OpenHABModule::handleAsyncUpdate()
{
	if (Engine::mainEngine == nullptr || Engine::mainEngine->isClearing) return;

	if (startRequested)
	{
		startRequested = false;
		if (eventThread == nullptr && enabled->boolValue() && !Engine::mainEngine->isLoadingFile) startConnection();
	}

	Array<Inbound> messages;
	{
		const ScopedLock sl(inboxLock);
		messages.swapWith(inbox);
	}

	const int gen = generation.load();
	bool receivedStates = false;

	for (auto& m : messages)
	{
		if (m.generation != gen) continue;

		switch (m.type)
		{
		case Inbound::SERVER_INFO:
			serverVersion = m.value;
			break;

		case Inbound::STRUCTURE:
			applyStructure(m.data);
			break;

		case Inbound::STATES:
			applyStates(m.data);
			receivedStates = true;
			break;

		case Inbound::STATE:
			if (ItemInfo* info = itemMap[m.name])
			{
				if (logIncomingData->boolValue()) NLOG(niceName, m.name << " = " << m.value);
				handleRemoteState(*info, m.stateType, m.value);
				receivedStates = true;
			}
			break;

		case Inbound::CONNECTED:
		{
			const bool wasConnected = isConnected->boolValue();
			isConnected->setValue(true);
			clearWarning();
			int count = 0;
			for (auto* info : items) if (!info->missing) count++;
			const String s = "Connected to openHAB " + serverVersion + ", " + String(count) + " items" + (skippedCount > 0 ? " (" + String(skippedCount) + " skipped)" : String());
			setStatus(s);
			if (!wasConnected) NLOG(niceName, s);
		}
		break;

		case Inbound::DISCONNECTED:
		{
			const bool wasConnected = isConnected->boolValue();
			isConnected->setValue(false);
			const String reason = m.value;
			setStatus("Not connected : " + reason + ", retrying in " + m.stateType + " s");
			if (wasConnected || getWarningMessage(warningNoId) != reason)
			{
				NLOGWARNING(niceName, (wasConnected ? "Lost the connection to openHAB : " : "Cannot connect to openHAB : ") << reason);
				setWarningMessage(reason, warningNoId, false);
			}
		}
		break;
		}
	}

	if (receivedStates) inActivityTrigger->trigger();
}

//==============================================================================
// Structure

OpenHABModule::ValuesBatch::ValuesBatch(OpenHABModule& m) :
	module(m),
	wasCollapsed(m.valuesCC.editorIsCollapsed),
	wasLoading(m.valuesCC.isCurrentlyLoadingData),
	nested(m.activeBatch != nullptr)
{
	if (nested) return;
	module.activeBatch = this;
	module.batchCreated.clear();
	module.batchStructureChanged = false;
	module.valuesCC.editorIsCollapsed = true;
	module.valuesCC.isCurrentlyLoadingData = true;
}

OpenHABModule::ValuesBatch::~ValuesBatch()
{
	if (nested) return;
	ControllableContainer& cc = module.valuesCC;
	cc.isCurrentlyLoadingData = wasLoading;
	cc.editorIsCollapsed = wasCollapsed;
	module.activeBatch = nullptr;

	for (auto& p : module.batchCreated) if (p != nullptr) p->hideInEditor = false;
	module.batchCreated.clear();

	if (module.batchStructureChanged)
	{
		cc.childStructureChanged(&cc); //the one structure notification the batch held back
		cc.queuedNotifier.addMessage(new ContainerAsyncEvent(ContainerAsyncEvent::ControllableContainerNeedsRebuild, &cc));
	}
}

OpenHABModule::ItemInfo* OpenHABModule::getOrCreateInfo(const String& name)
{
	if (ItemInfo* info = itemMap[name]) return info;
	ItemInfo* info = items.add(new ItemInfo());
	info->name = name;
	itemMap.set(name, info);
	return info;
}

String OpenHABModule::parameterTypeFor(OpenHAB::Kind kind)
{
	switch (kind)
	{
	case OpenHAB::Kind::SWITCH:
	case OpenHAB::Kind::CONTACT: return BoolParameter::getTypeStringStatic();
	case OpenHAB::Kind::DIMMER:
	case OpenHAB::Kind::ROLLERSHUTTER:
	case OpenHAB::Kind::NUMBER: return FloatParameter::getTypeStringStatic();
	case OpenHAB::Kind::COLOR: return ColorParameter::getTypeStringStatic();
	case OpenHAB::Kind::OPTIONS:
	case OpenHAB::Kind::PLAYER: return EnumParameter::getTypeStringStatic();
	case OpenHAB::Kind::STRING:
	case OpenHAB::Kind::DATETIME:
	case OpenHAB::Kind::LOCATION:
	case OpenHAB::Kind::CALL: return StringParameter::getTypeStringStatic();
	default: return String();
	}
}

OpenHAB::Kind OpenHABModule::kindFromCustomData(const var& customData)
{
	const String type = customData.getProperty("openhabType", "").toString();
	if (type.isEmpty()) return OpenHAB::Kind::NONE;
	String base = type.startsWith("Group:") ? type.fromFirstOccurrenceOf(":", false, false) : type;
	base = base.upToFirstOccurrenceOf(":", false, false);
	const bool hasOptions = (bool)customData.getProperty("hasOptions", false);
	return OpenHAB::kindForBaseType(base, hasOptions);
}

String OpenHABModule::describeItem(const ItemInfo& info) const
{
	String d;
	if (info.missing) d << "Not found in openHAB any more. ";
	d << "openHAB " << info.spec.type << " item " << info.name;
	if (info.spec.label.isNotEmpty()) d << " (\"" << info.spec.label << "\")";
	if (info.unit.isNotEmpty()) d << ", in " << info.unit;
	if (info.spec.readOnly) d << ", read-only";
	return d;
}

Parameter* OpenHABModule::createParameter(ItemInfo& info)
{
	const String nice = info.spec.label.isNotEmpty() ? info.spec.label : info.name;
	const String desc = describeItem(info);
	Parameter* p = nullptr;

	switch (info.kind)
	{
	case OpenHAB::Kind::SWITCH:
	case OpenHAB::Kind::CONTACT: p = new BoolParameter(nice, desc, false); break;
	case OpenHAB::Kind::DIMMER:
	case OpenHAB::Kind::ROLLERSHUTTER: p = new FloatParameter(nice, desc, 0, 0, 100); break;
	case OpenHAB::Kind::NUMBER: p = new FloatParameter(nice, desc, 0); break;
	case OpenHAB::Kind::COLOR: p = new ColorParameter(nice, desc, Colours::black); break;
	case OpenHAB::Kind::OPTIONS:
	case OpenHAB::Kind::PLAYER: p = new EnumParameter(nice, desc); break;
	case OpenHAB::Kind::STRING:
	case OpenHAB::Kind::DATETIME:
	case OpenHAB::Kind::LOCATION:
	case OpenHAB::Kind::CALL: p = new StringParameter(nice, desc, ""); break;
	default: return nullptr;
	}

	p->setCustomShortName(info.name); //the address is the item name, whatever the label says
	p->saveValueOnly = false; //saved with its definition, so targets resolve before openHAB is reachable
	p->forceSaveValue = true;
	p->isRemovableByUser = false;
	p->saveCustomData = true;

	if (activeBatch != nullptr)
	{
		p->hideInEditor = true;
		batchCreated.add(p);
		batchStructureChanged = true;
	}

	valuesCC.addParameter(p);
	return p;
}

void OpenHABModule::configureParameter(ItemInfo& info, Parameter* p)
{
	const String nice = info.spec.label.isNotEmpty() ? info.spec.label : info.name;
	if (p->niceName != nice) p->setNiceName(nice);
	p->description = describeItem(info);
	if (p->isControllableFeedbackOnly != info.spec.readOnly) p->setControllableFeedbackOnly(info.spec.readOnly);

	switch (info.kind)
	{
	case OpenHAB::Kind::DIMMER:
	case OpenHAB::Kind::ROLLERSHUTTER:
		if ((double)p->minimumValue != 0 || (double)p->maximumValue != 100) p->setRange(0, 100);
		break;

	case OpenHAB::Kind::NUMBER:
		//openHAB's minimum and maximum describe the control. A sensor can report outside them, and a
		//range would clamp what is shown, so read-only items stay unbounded.
		if (!info.spec.readOnly && info.spec.hasMinimum && info.spec.hasMaximum && info.spec.minimum < info.spec.maximum)
		{
			if ((double)p->minimumValue != info.spec.minimum || (double)p->maximumValue != info.spec.maximum) p->setRange(info.spec.minimum, info.spec.maximum);
			info.rangeFromServer = true;
		}
		else if (info.rangeFromServer)
		{
			p->clearRange();
			info.rangeFromServer = false;
		}
		break;

	case OpenHAB::Kind::OPTIONS:
	case OpenHAB::Kind::PLAYER:
		updateEnumOptions(info, static_cast<EnumParameter*>(p));
		break;

	default:
		break;
	}

	var cd(new DynamicObject());
	cd.getDynamicObject()->setProperty("openhabType", info.spec.isGroup ? "Group:" + info.spec.baseType + (info.spec.dimension.isNotEmpty() ? ":" + info.spec.dimension : String()) : info.spec.type);
	if (!info.spec.options.isEmpty()) cd.getDynamicObject()->setProperty("hasOptions", true);
	if (info.rangeFromServer) cd.getDynamicObject()->setProperty("rangeFromServer", true);
	p->customData = cd;
	p->saveCustomData = true;
}

void OpenHABModule::updateEnumOptions(ItemInfo& info, EnumParameter* ep)
{
	Array<OpenHAB::Option> opts;
	if (info.kind == OpenHAB::Kind::PLAYER)
	{
		for (auto& s : StringArray("PLAY", "PAUSE", "NEXT", "PREVIOUS", "REWIND", "FASTFORWARD")) opts.add({ s, s });
	}
	else opts = info.spec.options;

	StringArray keys;
	StringArray datas;
	for (auto& o : opts)
	{
		String key = o.label.isNotEmpty() ? o.label : o.value;
		if (keys.contains(key)) key << " (" << o.value << ")";
		keys.add(key);
		datas.add(o.value);
	}

	bool same = ep->enumValues.size() == keys.size();
	for (int i = 0; same && i < keys.size(); i++)
	{
		same = ep->enumValues[i]->key == keys[i] && ep->enumValues[i]->value.toString() == datas[i];
	}
	if (same) return;

	const String current = ep->getValueData().toString();
	ep->clearOptions();
	for (int i = 0; i < keys.size(); i++) ep->addOption(keys[i], datas[i], false);
	if (current.isNotEmpty() && !ep->setValueWithData(current))
	{
		ep->addOption(current, current, false);
		ep->setValueWithData(current);
	}
}

void OpenHABModule::ensureParameter(ItemInfo& info)
{
	Parameter* p = info.param.get();
	if (p == nullptr)
	{
		p = dynamic_cast<Parameter*>(valuesCC.getControllableByName(info.name, false, false));
		if (p != nullptr) info.rangeFromServer = (bool)p->customData.getProperty("rangeFromServer", false); //restored from the project
	}

	const String wanted = parameterTypeFor(info.kind);
	if (p != nullptr && p->getTypeString() != wanted)
	{
		NLOG(niceName, "Item " << info.name << " is now a " << info.spec.type << ", replacing its value (links to it reconnect by address)");
		reconcileList.removeAllInstancesOf(&info);
		info.reconcilePending = false;
		valuesCC.removeControllable(p);
		batchStructureChanged = true;
		p = nullptr;
		info.hasRemote = false;
		info.believed.clear();
	}

	if (p == nullptr) p = createParameter(info);
	info.param = p;
	if (p != nullptr) configureParameter(info, p);
}

void OpenHABModule::markMissing(ItemInfo& info)
{
	if (info.missing) return;
	info.missing = true;
	reconcileList.removeAllInstancesOf(&info);
	info.reconcilePending = false;

	if (Parameter* p = info.param.get())
	{
		p->description = describeItem(info);
		//the name only, never the address : links keep pointing at the item name
		if (!p->niceName.endsWith(" (missing)")) p->setNiceName(p->niceName + " (missing)");
	}
}

void OpenHABModule::applyStructure(const var& list)
{
	const Array<var>* dtos = list.getArray();
	if (dtos == nullptr) return;

	const ScopedValueSetter<bool> svs(applyingRemote, true);
	ValuesBatch batch(*this);

	OpenHAB::ItemFilter filter;
	filter.set(itemFilter->stringValue());
	const bool withReadOnly = includeReadOnly->boolValue();

	HashMap<String, int> usable;
	StringArray removedByFilter;
	int added = 0;
	skippedCount = 0;
	bool needsSort = false;

	for (auto& dto : *dtos)
	{
		OpenHAB::ItemSpec spec;
		if (!OpenHAB::parseItem(dto, spec)) continue;

		if (spec.kind == OpenHAB::Kind::NONE)
		{
			skippedCount++;
			continue;
		}

		if (!filter.matches(spec.name) || (spec.readOnly && !withReadOnly))
		{
			removedByFilter.add(spec.name);
			continue;
		}

		ItemInfo* info = getOrCreateInfo(spec.name);
		const bool isNew = info->param == nullptr;
		const String oldLabel = info->spec.label;
		info->spec = spec;
		info->kind = spec.kind;
		info->missing = false;
		ensureParameter(*info);
		usable.set(spec.name, 1);

		if (isNew) added++;
		if (isNew || oldLabel != spec.label) needsSort = true;

		if (spec.hasState) handleRemoteState(*info, String(), spec.state); //servers that ignore staticDataOnly send states along
	}

	for (auto& n : removedByFilter) removeItem(n);

	//values restored from the project that no usable item claimed
	for (auto* c : valuesCC.controllables)
	{
		Parameter* p = dynamic_cast<Parameter*>(c);
		if (p == nullptr || itemMap.contains(p->shortName)) continue;
		ItemInfo* info = getOrCreateInfo(p->shortName);
		info->param = p;
		info->kind = kindFromCustomData(p->customData);
		info->spec.name = p->shortName;
		info->spec.type = p->customData.getProperty("openhabType", "").toString();
		info->rangeFromServer = (bool)p->customData.getProperty("rangeFromServer", false);
	}

	StringArray nowMissing;
	for (auto* info : items)
	{
		if (!usable.contains(info->name) && !info->missing)
		{
			markMissing(*info);
			nowMissing.add(info->name);
		}
	}

	if (!nowMissing.isEmpty())
	{
		NLOGWARNING(niceName, nowMissing.size() << " item(s) are gone from openHAB or no longer of a type this module handles. Their values are kept until \"Remove Missing Items\" : " << nowMissing.joinIntoString(", ").substring(0, 500));
	}

	if (needsSort)
	{
		valuesCC.sortControllables();
		batchStructureChanged = true;
	}
	if (added > 0 && isConnected->boolValue()) NLOG(niceName, added << " new item(s)");
}

void OpenHABModule::removeItem(const String& name)
{
	if (ItemInfo* info = itemMap[name])
	{
		reconcileList.removeAllInstancesOf(info);
		if (Parameter* p = info->param.get())
		{
			valuesCC.removeControllable(p);
			batchStructureChanged = true;
		}
		itemMap.remove(name);
		items.removeObject(info);
	}
	else if (Controllable* c = valuesCC.getControllableByName(name, false, false))
	{
		valuesCC.removeControllable(c); //restored from the project, never claimed
		batchStructureChanged = true;
	}
}

void OpenHABModule::removeMissingItems()
{
	ValuesBatch batch(*this);
	StringArray names;
	for (auto* info : items) if (info->missing) names.add(info->name);
	for (auto& n : names) removeItem(n);
	if (!names.isEmpty()) NLOG(niceName, "Removed " << names.size() << " missing item(s)");
}

//==============================================================================
// States coming in

void OpenHABModule::applyStates(const var& list)
{
	const Array<var>* entries = list.getArray();
	if (entries == nullptr) return;

	for (auto& e : *entries)
	{
		const String name = e.getProperty("name", "").toString();
		if (ItemInfo* info = itemMap[name])
		{
			if (!e.hasProperty("state")) continue;
			handleRemoteState(*info, String(), e.getProperty("state", "").toString());
		}
	}
}

void OpenHABModule::handleRemoteState(ItemInfo& info, const String& stateType, const String& value)
{
	info.remoteType = stateType;
	info.remoteValue = value;
	info.hasRemote = true;

	if (info.missing || OpenHAB::isUndefinedState(stateType, value)) return; //NULL and UNDEF keep the last value

	if (info.reconcilePending)
	{
		//our own change is on its way, and a fading or moving device reports as it goes : wait until it is done
		info.lastRemoteWhilePending = Time::getMillisecondCounter() | 1;
		return;
	}

	applyRemote(info);
}

void OpenHABModule::applyRemote(ItemInfo& info)
{
	if (!info.hasRemote || OpenHAB::isUndefinedState(info.remoteType, info.remoteValue)) return;

	const ScopedValueSetter<bool> svs(applyingRemote, true);
	applyStateToParameter(info, info.remoteType, info.remoteValue);
	info.believed = info.remoteValue;
}

void OpenHABModule::applyStateToParameter(ItemInfo& info, const String& stateType, const String& value)
{
	Parameter* p = info.param.get();
	if (p == nullptr) return;

	switch (info.kind)
	{
	case OpenHAB::Kind::SWITCH:
		if (value == "ON") p->setValue(true);
		else if (value == "OFF") p->setValue(false);
		break;

	case OpenHAB::Kind::CONTACT:
		if (value == "OPEN") p->setValue(true);
		else if (value == "CLOSED") p->setValue(false);
		break;

	case OpenHAB::Kind::DIMMER:
	case OpenHAB::Kind::ROLLERSHUTTER:
	case OpenHAB::Kind::NUMBER:
	{
		double v = 0;
		String unit;
		if (value == "ON" || value == "DOWN") v = 100;
		else if (value == "OFF" || value == "UP") v = 0;
		else if (!OpenHAB::parseNumber(value, v, &unit)) break;

		if (unit.isNotEmpty() && unit != info.unit)
		{
			info.unit = unit;
			p->description = describeItem(info);
		}
		p->setValue(v);
	}
	break;

	case OpenHAB::Kind::COLOR:
	{
		double h = 0, s = 0, b = 0;
		if (OpenHAB::parseHSB(value, h, s, b))
		{
			//black carries no hue or saturation (a fade to black ends as 0,0,0), grey no hue : keep what came before
			if (b > 0.05)
			{
				if (s > 0.05 || !info.hasHSB) info.hue = h;
				info.saturation = s;
				info.hasHSB = true;
			}
			double r, g, bl;
			OpenHAB::hsbToRgb(h, s / 100.0, b / 100.0, r, g, bl);
			var c;
			c.append((float)r);
			c.append((float)g);
			c.append((float)bl);
			c.append(1.0f);
			p->setValue(c);
		}
	}
	break;

	case OpenHAB::Kind::OPTIONS:
	case OpenHAB::Kind::PLAYER:
	{
		EnumParameter* ep = static_cast<EnumParameter*>(p);
		if (!ep->setValueWithData(value))
		{
			//a state openHAB describes no option for : show it anyway rather than keep a stale one
			ep->addOption(value, value, false);
			ep->setValueWithData(value);
		}
	}
	break;

	case OpenHAB::Kind::STRING:
	case OpenHAB::Kind::DATETIME:
	case OpenHAB::Kind::LOCATION:
	case OpenHAB::Kind::CALL:
		p->setValue(value);
		break;

	default:
		break;
	}

	ignoreUnused(stateType);
}

//==============================================================================
// Commands going out

void OpenHABModule::valueChanged(Parameter* p)
{
	if (applyingRemote) return;
	if (isCurrentlyLoadingData || valuesCC.isCurrentlyLoadingData) return;
	if (Engine::mainEngine == nullptr || Engine::mainEngine->isLoadingFile || Engine::mainEngine->isClearing) return;
	if (!enabled->boolValue() || !sendCommands->boolValue()) return;

	ItemInfo* info = itemMap[p->shortName];
	if (info == nullptr || info->param != p) return;

	if (info->missing)
	{
		if (warnOnce(*info)) NLOGWARNING(niceName, info->name << " does not exist in openHAB any more, not sending");
		return;
	}

	if (info->spec.readOnly || OpenHAB::kindIsReadOnly(info->kind))
	{
		if (warnOnce(*info)) NLOGWARNING(niceName, info->name << " is read-only in openHAB, not sending");
		return;
	}

	const String command = commandFromParameter(*info);
	if (command.isEmpty() && info->kind != OpenHAB::Kind::STRING) return; //an empty text is a valid String command, nothing else is

	//"Always Notify" on a value means : send even when openHAB already holds it
	if (!p->alwaysNotify && info->hasRemote && sameCommand(*info, command, info->believed, false)) return;

	queueCommand(*info, command);
}

bool OpenHABModule::warnOnce(ItemInfo& info)
{
	const uint32 now = Time::getMillisecondCounter();
	if (info.lastWarning != 0 && (int)(now - info.lastWarning) < 5000) return false;
	info.lastWarning = now | 1;
	return true;
}

String OpenHABModule::commandFromParameter(ItemInfo& info)
{
	Parameter* p = info.param.get();
	if (p == nullptr) return String();

	switch (info.kind)
	{
	case OpenHAB::Kind::SWITCH:
		return p->boolValue() ? "ON" : "OFF";

	case OpenHAB::Kind::DIMMER:
	case OpenHAB::Kind::ROLLERSHUTTER:
		return OpenHAB::formatNumber(jlimit(0.0, 100.0, (double)p->getValue()), info.spec.step);

	case OpenHAB::Kind::NUMBER:
		return OpenHAB::formatNumber((double)p->getValue(), info.spec.step);

	case OpenHAB::Kind::COLOR:
	{
		var v = p->getValue();
		if (!v.isArray() || v.size() < 3) return String();

		double h, s, b;
		OpenHAB::rgbToHsb((double)v[0], (double)v[1], (double)v[2], h, s, b);
		b *= v.size() > 3 ? jlimit(0.0, 1.0, (double)v[3]) : 1.0; //alpha dims

		//black and grey carry no hue : keep the light's own, so fading out and back in keeps the colour
		if (b < 0.0005 && info.hasHSB)
		{
			h = info.hue;
			s = info.saturation / 100.0;
		}
		else if (s < 0.0005 && info.hasHSB) h = info.hue;

		return OpenHAB::formatHSB(h, s * 100.0, b * 100.0);
	}

	case OpenHAB::Kind::OPTIONS:
	case OpenHAB::Kind::PLAYER:
		return static_cast<EnumParameter*>(p)->getValueData().toString();

	case OpenHAB::Kind::STRING:
	case OpenHAB::Kind::DATETIME:
	case OpenHAB::Kind::LOCATION:
		return p->stringValue();

	default:
		return String();
	}
}

//asEcho compares what was sent with what openHAB reports back : a binding that drives 8-bit DMX quantises a
//percentage to 1/255, so 50 comes back as 49.804, and that is the same value, not a disagreement.
bool OpenHABModule::sameCommand(const ItemInfo& info, const String& a, const String& b, bool asEcho) const
{
	switch (info.kind)
	{
	case OpenHAB::Kind::DIMMER:
	case OpenHAB::Kind::ROLLERSHUTTER:
	case OpenHAB::Kind::NUMBER:
	{
		double x, y;
		if (!OpenHAB::parseNumber(a, x) || !OpenHAB::parseNumber(b, y)) return a == b;
		if (asEcho && info.kind != OpenHAB::Kind::NUMBER && std::abs(x - y) <= 0.5) return true;
		return OpenHAB::sameNumber(x, y, info.spec.step);
	}

	case OpenHAB::Kind::COLOR:
		return OpenHAB::sameHSB(a, b, asEcho ? 0.5 : 0.01);

	default:
		return a == b;
	}
}

void OpenHABModule::queueCommand(ItemInfo& info, const String& command)
{
	const uint32 now = Time::getMillisecondCounter();
	info.believed = command;

	if (info.kind == OpenHAB::Kind::COLOR)
	{
		double h, s, b;
		if (OpenHAB::parseHSB(command, h, s, b) && b > 0.05)
		{
			if (s > 0.05) info.hue = h;
			info.saturation = s;
			info.hasHSB = true;
		}
	}

	if (echoWindow->intValue() > 0)
	{
		info.lastLocalChange = now;
		if (!info.reconcilePending)
		{
			info.reconcilePending = true;
			reconcileList.addIfNotAlreadyThere(&info);
		}
		if (!isTimerRunning()) startTimer(50);
	}

	{
		const ScopedLock sl(queueLock);
		bool merged = false;
		for (int i = commandQueue.size() - 1; i >= 0; --i)
		{
			PendingCommand& c = commandQueue.getReference(i);
			if (c.item != info.name) continue;
			if (c.coalesce)
			{
				c.command = command;
				c.queuedAt = now;
				merged = true;
			}
			break; //only the newest entry for an item can be merged, older ones must go out in order
		}

		if (!merged) commandQueue.add({ info.name, command, now, true, generation.load() });
		if (commandQueue.size() > OpenHABModuleConstants::maxQueuedCommands) commandQueue.removeRange(0, commandQueue.size() - OpenHABModuleConstants::maxQueuedCommands);
	}

	commandWake.signal();
	outActivityTrigger->trigger();
}

void OpenHABModule::queueRawCommand(const String& itemName, const String& command)
{
	if (!enabled->boolValue() || !sendCommands->boolValue()) return;

	if (!OpenHAB::isValidItemName(itemName))
	{
		NLOGWARNING(niceName, "\"" << itemName << "\" is not an openHAB item name");
		return;
	}

	if (command.isEmpty())
	{
		NLOGWARNING(niceName, "Empty command for " << itemName << ", not sending");
		return;
	}

	{
		const ScopedLock sl(queueLock);
		commandQueue.add({ itemName, command, Time::getMillisecondCounter(), false, generation.load() });
		if (commandQueue.size() > OpenHABModuleConstants::maxQueuedCommands) commandQueue.removeRange(0, commandQueue.size() - OpenHABModuleConstants::maxQueuedCommands);
	}

	commandWake.signal();
}

bool OpenHABModule::hasQueuedCommand(const String& itemName)
{
	const ScopedLock sl(queueLock);
	for (auto& c : commandQueue) if (c.item == itemName) return true;
	return false;
}

bool OpenHABModule::takeNextCommand(int gen, PendingCommand& out, int& waitMs, HashMap<String, uint32>& lastSent, int& expired)
{
	const ScopedLock sl(queueLock);
	const uint32 now = Time::getMillisecondCounter();
	const uint32 minInterval = (uint32)jmax(0.0, minCommandIntervalMs.load());

	expired = 0;
	for (int i = commandQueue.size() - 1; i >= 0; --i)
	{
		const PendingCommand& c = commandQueue.getReference(i);
		if (c.generation < gen) commandQueue.remove(i); //meant for a connection that was replaced
		else if (c.generation == gen && (int)(now - c.queuedAt) > OpenHABModuleConstants::commandExpiryMs)
		{
			commandQueue.remove(i);
			expired++;
		}
	}

	waitMs = 250;
	StringArray blocked; //an item whose next command is not due yet holds back its later ones too
	for (int i = 0; i < commandQueue.size(); i++)
	{
		const PendingCommand& c = commandQueue.getReference(i);
		if (c.generation != gen || blocked.contains(c.item)) continue;

		const int until = lastSent.contains(c.item) ? (int)(lastSent[c.item] + minInterval - now) : 0;
		if (until <= 0)
		{
			out = c;
			commandQueue.remove(i);
			return true;
		}

		blocked.add(c.item);
		waitMs = jmin(waitMs, until);
	}

	return false;
}

void OpenHABModule::requeueCommand(const PendingCommand& cmd)
{
	const ScopedLock sl(queueLock);
	if (cmd.coalesce)
	{
		for (auto& c : commandQueue) if (c.item == cmd.item && c.coalesce) return; //a newer value already replaces it
	}
	commandQueue.insert(0, cmd);
}

//Once openHAB has said nothing new about the item for the echo window and nothing is left to send, its last
//reported state wins if it disagrees with the value : a device that clamped, refused or never confirmed must not
//leave the value lying.
void OpenHABModule::timerCallback()
{
	reapRetiredThreads(false);

	const uint32 now = Time::getMillisecondCounter();
	const int window = echoWindow->intValue();
	const int maxHold = jmax(10000, window * 4); //an item openHAB updates without pause must not freeze the value

	for (int i = reconcileList.size() - 1; i >= 0; --i)
	{
		ItemInfo* info = reconcileList[i];
		const int sinceLocal = (int)(now - info->lastLocalChange);
		const int sinceRemote = info->lastRemoteWhilePending != 0 ? (int)(now - info->lastRemoteWhilePending) : sinceLocal;
		const bool quiet = sinceLocal >= window && sinceRemote >= window;
		if ((!quiet && sinceLocal < maxHold) || hasQueuedCommand(info->name)) continue;

		reconcileList.remove(i);
		info->reconcilePending = false;
		info->lastRemoteWhilePending = 0;

		if (!info->hasRemote || info->missing || OpenHAB::isUndefinedState(info->remoteType, info->remoteValue)) continue;

		const String current = commandFromParameter(*info);
		if (current.isEmpty() || !sameCommand(*info, current, info->remoteValue, true)) applyRemote(*info);
		else info->believed = info->remoteValue;
	}

	if (reconcileList.isEmpty() && retiredThreads.isEmpty()) stopTimer();
}

var OpenHABModule::sendCommandFromScript(const var::NativeFunctionArgs& args)
{
	OpenHABModule* m = getObjectFromJS<OpenHABModule>(args);
	if (m == nullptr) return var();

	if (args.numArguments < 2)
	{
		NLOGWARNING(m->niceName, "sendCommand takes an item name and a command");
		return var();
	}

	m->queueRawCommand(args.arguments[0].toString(), args.arguments[1].toString());
	return var();
}
