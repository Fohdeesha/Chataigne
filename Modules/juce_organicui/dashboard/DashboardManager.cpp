/*
  ==============================================================================

	DashboardManager.cpp
	Created: 19 Apr 2017 10:57:53pm
	Author:  Ben

  ==============================================================================
*/

#include "JuceHeader.h"

juce_ImplementSingleton(DashboardManager)

ApplicationProperties& getAppProperties();
String getAppVersion();

DashboardManager::DashboardManager() :
	BaseManager("Dashboards")
{
	hideInRemoteControl = true;
	defaultHideInRemoteControl = true;

	editMode = addBoolParameter("Edit Mode", "If checked, items are editable. If not, items are normally usable", true);
	snapping = addBoolParameter("Snapping", "If checked, items are automatically aligned when dragging them closed to other ones", true);
	connectedClients = addIntParameter("Connected Clients", "Number of clients connected to the web dashboard", 0);
	connectedClients->setControllableFeedbackOnly(true);

	tabsBGColor = addColorParameter("Tabs BG Color", "Color for the tabs in the web view", NORMAL_COLOR);
	tabsLabelColor = addColorParameter("Tabs Label Color", "Color for the tabs in the web view", TEXT_COLOR);
	tabsBorderColor = addColorParameter("Tabs Border Color", "Color for the tabs in the web view", Colours::black);
	tabsBorderWidth = addFloatParameter("Tabs Border Width", "Width for the border of tabs in the web view", 0, 0);
	tabsSelectedBGColor = addColorParameter("Tabs Selected BG Color", "Color for the tabs in the web view", HIGHLIGHT_COLOR);
	tabsSelectedLabelColor = addColorParameter("Tabs Selected Label Color", "Color for the tabs in the web view", Colours::black);
	tabsSelectedBorderColor = addColorParameter("Tabs Selected Border Color", "Color for the tabs in the web view", Colours::black);
	tabsSelectedBorderWidth = addFloatParameter("Tabs Selected Border Width", "Width for the border of tabs in the web view", 0, 0);

#if ORGANICUI_USE_WEBSERVER
	serverRootPath = File::getSpecialLocation(File::SpecialLocationType::userDocumentsDirectory).getChildFile(OrganicApplication::getInstance()->getApplicationName() + "/dashboard");
#endif
}

DashboardManager::~DashboardManager()
{
	for (auto& i : items) i->removeDashboardListener(this);

#if ORGANICUI_USE_WEBSERVER
	if (server != nullptr)
	{
		server->stop();
		server.reset();
	}
#endif

#if ORGANICUI_USE_SERVUS
	servusThread.stopThread(1000);
#endif

	DashboardItemFactory::deleteInstance();

	Engine::mainEngine->removeEngineListener(this);
}

#if ORGANICUI_USE_WEBSERVER
void DashboardManager::setupServer()
{
	server.reset();
	connectedClients->setValue(0);

	if (Engine::mainEngine->isClearing) return;

	if (isCurrentlyLoadingData || Engine::mainEngine->isLoadingFile)
	{
		Engine::mainEngine->addEngineListener(this);
		return;
	}

	if (!ProjectSettings::getInstance()->enableServer->boolValue())
	{
		LOG("Dashboard server is not running");
#if ORGANICUI_USE_SERVUS
		servusThread.stopThread(1000);
#endif

		return;
	}

	int port = ProjectSettings::getInstance()->serverPort->intValue();

	/*
	File k = File::getSpecialLocation(File::currentApplicationFile).getParentDirectory().getChildFile("server.key");
	File c = File::getSpecialLocation(File::currentApplicationFile).getParentDirectory().getChildFile("server.crt");

	if (k.existsAsFile() && c.existsAsFile())
	{
		try
		{
			server.reset(new SecureWebSocketServer(c.getFullPathName(), k.getFullPathName()));
		}
		catch (std::exception e)
		{
			NLOGERROR(niceName, "Error creating secure server : " << e.what());
			return;
		}
	}
	else
	{
	*/
	server.reset(new SimpleWebSocketServer());
	//}

	server->addHTTPRequestHandler(this);

	File f = Engine::mainEngine->getFile();
	File serverLocalPath;
	if (f.existsAsFile()) serverLocalPath = f.getParentDirectory().getChildFile("dashboard");
	if (serverLocalPath.exists() && serverLocalPath.isDirectory())
	{
		LOG("Found local dashboard in project's folder, using this one.");
		server->rootPath = serverLocalPath;
	}
	else server->rootPath = serverRootPath;

	server->addWebSocketListener(this);
	server->start(port);

#if ORGANICUI_USE_SERVUS
	servusThread.setupZeroconf();
#endif

	LOG("Dashboard server is running on port " << port << " on IPs :\n" + NetworkHelpers::getLocalIPs().joinIntoString("\n"));
}

//Runs f on the message thread and waits for it. Gives up when the server is being stopped (stop() runs on the message
//thread and waits for this thread) or after 10 s, so it can never hang either side ; f must only use what it captured.
static bool runOnMessageThreadAndWait(std::function<void()> f)
{
	struct Job { WaitableEvent done; };
	auto job = std::make_shared<Job>();
	MessageManager::callAsync([job, f]()
		{
			f();
			job->done.signal();
		});

	int waitedMs = 0;
	while (!job->done.wait(50))
	{
		waitedMs += 50;
		if (Thread::currentThreadShouldExit() || waitedMs >= 10000) return false;
	}
	return true;
}

void DashboardManager::connectionOpened(const String& id)
{
	//the server's thread : the count and the resync on the message thread, where `server` is not being replaced
	MessageManager::callAsync([id]()
		{
			DashboardManager* m = getInstanceWithoutCreating();
			if (m == nullptr || m->server == nullptr) return;
			LOG("New browser connection to the Dashboard from " << id);
			m->connectedClients->setValue(m->server->getNumActiveConnections());

			//A browser loads the dashboards once, with the page : one that reconnects (Wi-Fi drop, tablet asleep) kept
			//showing the values of before. The full data is pushed to every new connection (the web client applies a
			//"dataType" : "all" message ; the page load then gets it twice, the same values).
			m->server->sendTo(m->buildServerDataString(), id);
		});
}

void DashboardManager::messageReceived(const String& id, const String& message)
{
	var data;
	Result result = JSON::parse(message, data);

	if (result.failed())
	{
		DBG("Error parsing: " << message << ", error : " << result.getErrorMessage());
		return;
	}

	//at most 1000 waiting : a flood from a client cannot grow the message queue without end
	if (pendingWebMessages.load() >= 1000) return;
	pendingWebMessages++;

	MessageManager::callAsync([id, data]()
		{
			if (DashboardManager* m = getInstanceWithoutCreating())
			{
				m->pendingWebMessages--;
				m->handleWebMessage(id, data);
			}
		});
}

static DashboardManager::WebAccess getWebAccessIn(DashboardItemManager& im, Controllable* c)
{
	DashboardManager::WebAccess result = DashboardManager::WEB_NONE;
	if (c == im.bgImage) result = DashboardManager::WEB_READ;

	for (auto& i : im.items)
	{
		if (DashboardControllableItem* ci = dynamic_cast<DashboardControllableItem*>(i))
		{
			if (ci->controllable.get() != c) continue;
			if (!ci->forceReadOnly->boolValue() && !c->isControllableFeedbackOnly) return DashboardManager::WEB_WRITE;
			result = DashboardManager::WEB_READ;
		}
		else if (DashboardCCItem* cci = dynamic_cast<DashboardCCItem*>(i))
		{
			ControllableContainer* root = cci->container.get();
			if (root == nullptr || c->hideInRemoteControl) continue;

			//inside the item's container, through containers the web client lists (it skips hidden ones)
			bool inside = false;
			for (ControllableContainer* p = c->parentContainer.get(); p != nullptr; p = p->parentContainer.get())
			{
				if (p == root)
				{
					inside = true;
					break;
				}
				if (p->hideInRemoteControl) break;
			}
			if (!inside) continue;

			if (!c->isControllableFeedbackOnly) return DashboardManager::WEB_WRITE;
			result = DashboardManager::WEB_READ;
		}
		else if (DashboardGroupItem* g = dynamic_cast<DashboardGroupItem*>(i))
		{
			DashboardManager::WebAccess a = getWebAccessIn(g->itemManager, c);
			if (a == DashboardManager::WEB_WRITE) return a;
			if (a == DashboardManager::WEB_READ) result = a;
		}
	}

	return result;
}

DashboardManager::WebAccess DashboardManager::getWebAccess(Controllable* c)
{
	if (c == nullptr) return WEB_NONE;
	WebAccess result = WEB_NONE;
	for (auto& d : items)
	{
		WebAccess a = getWebAccessIn(d->itemManager, c);
		if (a == WEB_WRITE) return a;
		if (a == WEB_READ) result = a;
	}
	return result;
}

void DashboardManager::handleWebMessage(const String& id, var data)
{
	if (Engine::mainEngine == nullptr || Engine::mainEngine->isClearing) return;

	if (data.hasProperty("setDashboard"))
	{
		if (Dashboard* d = getItemWithName(data.getProperty("setDashboard", ""), true))
		{
			bool sClients = data.getProperty("setInClients", true);
			bool sNative = data.getProperty("setInNative", true);
			setCurrentDashboard(d, sClients, sNative, id);
		}
		return;
	}

	String add = data.getProperty("controlAddress", "");
	if (add.isNotEmpty())
	{
		Controllable* c = Engine::mainEngine->getControllableForAddress(add);
		if (c == nullptr)
		{
			DBG("Controllable not found for address " << add);
		}
		else if (getWebAccess(c) != WEB_WRITE)
		{
			const uint32 now = Time::getMillisecondCounter();
			if (now - lastRefusedWebLog >= 5000)
			{
				lastRefusedWebLog = now;
				LOGWARNING("Dashboard : " << id << " tried to change " << add << ", which no dashboard shows as editable. Refused.");
			}
		}
		else
		{
			switch (c->type)
			{
			case Controllable::TRIGGER:
				((Trigger*)c)->trigger();
				break;

			default: //Parameter
			{
				var val = data.getProperty("value", var());
				if (!val.isVoid())
				{
					((Parameter*)c)->setValue(val);
				}
			}
			break;
			}
		}
	}
}

void DashboardManager::connectionClosed(const String& id, int status, const String& reason)
{
	MessageManager::callAsync([id]()
		{
			DashboardManager* m = getInstanceWithoutCreating();
			if (m == nullptr || m->server == nullptr) return;
			LOG("Connection to the Dashboard closed by " << id);
			m->connectedClients->setValue(m->server->getNumActiveConnections());
		});
}

void DashboardManager::connectionError(const String& id, int status, const String& errorMessage)
{
	MessageManager::callAsync([]()
		{
			DashboardManager* m = getInstanceWithoutCreating();
			if (m == nullptr || m->server == nullptr) return;
			m->connectedClients->setValue(m->server->getNumActiveConnections());
		});
}

var DashboardManager::getServerData()
{
	var data(new DynamicObject());
	data.getDynamicObject()->setProperty("dataType", "all");
	data.getDynamicObject()->setProperty("appName", OrganicApplication::getInstance()->getApplicationName());
	data.getDynamicObject()->setProperty("appVersion", OrganicApplication::getInstance()->getApplicationVersion());
	data.getDynamicObject()->setProperty("osName", SystemStats::getOperatingSystemName());
	data.getDynamicObject()->setProperty("osType", SystemStats::getOperatingSystemType());
	data.getDynamicObject()->setProperty("computerName", SystemStats::getComputerName());
	data.getDynamicObject()->setProperty("userName", SystemStats::getFullUserName());

	if (ProjectSettings::getInstance()->dashboardPassword != nullptr)
	{
		String pass = ProjectSettings::getInstance()->dashboardPassword->stringValue();
		if (pass.isNotEmpty())
		{
			data.getDynamicObject()->setProperty("password", pass);
			data.getDynamicObject()->setProperty("unlockOnce", ProjectSettings::getInstance()->unlockOnce->boolValue());
		}
	}

	var iData;
	for (auto& d : items)
	{
		iData.append(d->getServerData());
	}
	data.getDynamicObject()->setProperty("items", iData);
	return data;
}


String DashboardManager::buildServerDataString()
{
	var data = getServerData();

	var tabsData(new DynamicObject());
	tabsData.getDynamicObject()->setProperty("bgColor", tabsBGColor->value);
	tabsData.getDynamicObject()->setProperty("labelColor", tabsLabelColor->value);
	tabsData.getDynamicObject()->setProperty("borderColor", tabsBorderColor->value);
	tabsData.getDynamicObject()->setProperty("borderWidth", tabsBorderWidth->floatValue());
	tabsData.getDynamicObject()->setProperty("bgColorSelected", tabsSelectedBGColor->value);
	tabsData.getDynamicObject()->setProperty("labelColorSelected", tabsSelectedLabelColor->value);
	tabsData.getDynamicObject()->setProperty("borderColorSelected", tabsSelectedBorderColor->value);
	tabsData.getDynamicObject()->setProperty("borderWidthSelected", tabsSelectedBorderWidth->floatValue());
	data.getDynamicObject()->setProperty("tabs", tabsData);

	return JSON::toString(data, true);
}

bool DashboardManager::handleHTTPRequest(std::shared_ptr<HttpServer::Response> response, std::shared_ptr<HttpServer::Request> request)
{
	String dataStr;
	SimpleWeb::CaseInsensitiveMultimap header;

	if (String(request->path) == "/data")
	{
		//built on the message thread : the server's thread walked the dashboards while a load replaced them
		auto text = std::make_shared<String>();
		if (!runOnMessageThreadAndWait([text]() { if (DashboardManager* m = getInstanceWithoutCreating()) *text = m->buildServerDataString(); }))
		{
			header.emplace("Access-Control-Allow-Origin", "*");
			response->write(SimpleWeb::StatusCode::server_error_service_unavailable, "The application is busy, try again", header);
			return true;
		}

		header.emplace("Content-Type", "application/json");
		dataStr = *text;
	}
	else if (String(request->path) == "/fileData")
	{
		SimpleWeb::CaseInsensitiveMultimap query = request->parse_query_string();

		// If key not found in map iterator to end is returned
		auto arg = query.find("controlAddress");
		if (arg == query.end())
		{
			header.emplace("Content-Type", "text/html");
			dataStr = "Missing controlAddress argument in query";
		}
		else
		{
			//Resolved on the message thread, and only for a file a dashboard shows : any File parameter's file (a script,
			//a show file) could be downloaded by its address
			const String address(arg->second);
			auto file = std::make_shared<File>();
			if (!runOnMessageThreadAndWait([file, address]()
				{
					DashboardManager* m = getInstanceWithoutCreating();
					if (m == nullptr || Engine::mainEngine == nullptr) return;
					FileParameter* fp = dynamic_cast<FileParameter*>(Engine::mainEngine->getControllableForAddress(address));
					if (fp != nullptr && m->getWebAccess(fp) != WEB_NONE) *file = fp->getFile();
				}))
			{
				header.emplace("Access-Control-Allow-Origin", "*");
				response->write(SimpleWeb::StatusCode::server_error_service_unavailable, "The application is busy, try again", header);
				return true;
			}

			if (file->existsAsFile()) server->serveFile(*file, response);
			else
			{
				DBG(address << " has not been found, is not a File Parameter or is not on a dashboard");
				*response << "HTTP/1.1 404 Not Found";
			}

			return true;
		}
	}
	else if (customHandleHTTPRequestFunc)
	{
		dataStr = customHandleHTTPRequestFunc(response, request, header);
	}



	if (dataStr.isNotEmpty())
	{
		header.emplace("Content-Length", String(dataStr.getNumBytesAsUTF8()).toStdString()); //bytes, not characters (JSON::toString is ASCII, a custom handler's text may not be)
		header.emplace("Accept-range", "bytes");
		header.emplace("Access-Control-Allow-Origin", "*");

		response->write(SimpleWeb::StatusCode::success_ok, header);
		*response << dataStr;

		return true;
	}

	return false;
}

#if SIMPLEWEB_SECURE_SUPPORTED
bool DashboardManager::handleHTTPSRequest(std::shared_ptr<HttpsServer::Response> response, std::shared_ptr<HttpsServer::Request> request)
{
	String dataStr;
	SimpleWeb::CaseInsensitiveMultimap header;

	if (String(request->path) == "/data")
	{
		auto text = std::make_shared<String>();
		if (!runOnMessageThreadAndWait([text]() { if (DashboardManager* m = getInstanceWithoutCreating()) *text = m->buildServerDataString(); }))
		{
			header.emplace("Access-Control-Allow-Origin", "*");
			response->write(SimpleWeb::StatusCode::server_error_service_unavailable, "The application is busy, try again", header);
			return true;
		}
		dataStr = *text;

		header.emplace("Content-Length", String(dataStr.getNumBytesAsUTF8()).toStdString());
		header.emplace("Content-Type", "application/json");
		header.emplace("Accept-range", "bytes");
		header.emplace("Access-Control-Allow-Origin", "*");

		response->write(SimpleWeb::StatusCode::success_ok, header);
		*response << dataStr;

		return true;
	}
	else if (String(request->path) == "/fileData")
	{
		SimpleWeb::CaseInsensitiveMultimap query = request->parse_query_string();

		// If key not found in map iterator to end is returned
		auto arg = query.find("controlAddress");
		if (arg == query.end())
		{
			header.emplace("Content-Type", "text/html");
			dataStr = "Missing controlAddress argument in query";
		}
		else
		{
			const String address(arg->second);
			auto file = std::make_shared<File>();
			if (!runOnMessageThreadAndWait([file, address]()
				{
					DashboardManager* m = getInstanceWithoutCreating();
					if (m == nullptr || Engine::mainEngine == nullptr) return;
					FileParameter* fp = dynamic_cast<FileParameter*>(Engine::mainEngine->getControllableForAddress(address));
					if (fp != nullptr && m->getWebAccess(fp) != WEB_NONE) *file = fp->getFile();
				}))
			{
				header.emplace("Access-Control-Allow-Origin", "*");
				response->write(SimpleWeb::StatusCode::server_error_service_unavailable, "The application is busy, try again", header);
				return true;
			}

			if (file->existsAsFile()) server->serveFile(*file, response);
			else
			{
				DBG(address << " has not been found, is not a File Parameter or is not on a dashboard");
				*response << "HTTP/1.1 404 Not Found";
			}

			return true;
		}
	}

	return false;
}
#endif

void DashboardManager::setupDownloadURL(const String& _downloadURL)
{
	downloadURL = _downloadURL;
	if (!serverRootPath.exists()) downloadDashboardFiles();
}

void DashboardManager::downloadDashboardFiles()
{
	if (downloadURL.isEmpty())
	{
		DBG("No download URL for dashboard, exiting");
		return;
	}

	LOG("Downloading dashboard files...");
	downloadedFileZip = File::getSpecialLocation(File::userDocumentsDirectory).getChildFile(OrganicApplication::getInstance()->getApplicationName() + "/dashboard.zip");
	downloadTask = URL(downloadURL).downloadToFile(downloadedFileZip, URL::DownloadTaskOptions().withListener(this));

	if (downloadTask == nullptr)
	{
		LOGERROR("Error downloading dashboard files");
	}
}

void DashboardManager::progress(URL::DownloadTask* task, juce::int64 bytesDownloaded, int64 bytesTotal)
{
}

void DashboardManager::finished(URL::DownloadTask* task, bool success)
{
	if (success) LOG("Dashboard downloaded. Extracting to " << serverRootPath.getFullPathName());
	else
	{
		LOGERROR("Dashboard download error");
		return;
	}

	if (serverRootPath.exists()) serverRootPath.deleteRecursively();

	ZipFile zf(downloadedFileZip);
	zf.uncompressTo(serverRootPath);
	downloadedFileZip.deleteFile();

	LOG("You got a new dashboard my friend !");// << zf.getNumEntries() << " files downloaded to " << serverRootPath.getFullPathName());
}
#endif //ORGANICUI_USE_WEBSERVER


void DashboardManager::addItemInternal(Dashboard* item, var data)
{
	item->addDashboardListener(this);
	askForRefresh(nullptr);
}

void DashboardManager::removeItemInternal(Dashboard* item)
{
	item->removeDashboardListener(this);
	askForRefresh(nullptr);
}

void DashboardManager::setCurrentDashboard(Dashboard* d, bool setInClients, bool setInNative, StringArray excludeIds)
{
	if (d == nullptr) return;

	if (setInNative)
	{
		if (DashboardManagerView* v = ShapeShifterManager::getInstance()->getContentForType<DashboardManagerView>())
		{
			//the view (a panel that can be closed) and the dashboard (deleted, or gone with a load) may not outlive the post
			Component::SafePointer<DashboardManagerView> safeView(v);
			WeakReference<Inspectable> weakDashboard(d);
			MessageManager::getInstance()->callAsync([safeView, weakDashboard]()
				{
					if (safeView == nullptr || weakDashboard.wasObjectDeleted()) return;
					if (Dashboard* dd = dynamic_cast<Dashboard*>(weakDashboard.get())) safeView->setCurrentDashboard(dd);
				});
		}
	}

#if ORGANICUI_USE_WEBSERVER
	if (setInClients && server != nullptr && server->getNumActiveConnections() > 0)
	{
		var data(new DynamicObject());
		data.getDynamicObject()->setProperty("setDashboard", d->shortName);
		if (excludeIds.isEmpty()) server->send(JSON::toString(data));
		else server->sendExclude(JSON::toString(data), excludeIds);
	}
#endif
}

void DashboardManager::parameterFeedback(var data)
{
#if ORGANICUI_USE_WEBSERVER
	if (server != nullptr && server->getNumActiveConnections() > 0)
	{
		data.getDynamicObject()->setProperty("dataType", "feedback");
		server->send(JSON::toString(data));
	}
#endif
}

void DashboardManager::dashboardFeedback(var data)
{
#if ORGANICUI_USE_WEBSERVER
	if (server != nullptr && server->getNumActiveConnections() > 0)
	{
		data.getDynamicObject()->setProperty("dataType", "dashboardFeedback");
		server->send(JSON::toString(data));
	}
#endif
}


void DashboardManager::askForRefresh(Dashboard* d)
{
#if ORGANICUI_USE_WEBSERVER
	if (server != nullptr && server->getNumActiveConnections() > 0)
	{
		var data(new DynamicObject());
		data.getDynamicObject()->setProperty("refresh", d != nullptr ? d->shortName : "*");
		server->send(JSON::toString(data));
	}
#endif
}

void DashboardManager::fileLoaded()
{
	Engine::mainEngine->removeEngineListener(this);

#if ORGANICUI_USE_WEBSERVER
	setupServer();
#endif
}


#if ORGANICUI_USE_SERVUS
ServusThread::ServusThread() :
	Thread("Dashboard Zeroconf"),
	servus("_http._tcp")
{
}

void ServusThread::setupZeroconf()
{
	if (ProjectSettings::getInstanceWithoutCreating() == nullptr || ProjectSettings::getInstance()->serverPort == nullptr) return;

	if (Engine::mainEngine != nullptr && Engine::mainEngine->isClearing) return;
	if (!isThreadRunning()) startThread();
}


void ServusThread::run()
{
	String nameToAdvertise = OrganicApplication::getInstance()->getApplicationName() + " - Dashboard";
	int port = ProjectSettings::getInstance()->serverPort->intValue();
	int portToAdvertise = 0;
	while (portToAdvertise != port && !threadShouldExit())
	{
		portToAdvertise = port;
		servus.withdraw();
		servus.announce(portToAdvertise, nameToAdvertise.toStdString());

		if (port != portToAdvertise)
		{
			DBG("Name or port changed during advertise, readvertising");
		}
	}

	LOG("Dashboard Zeroconf service created : " << nameToAdvertise << ":" << portToAdvertise);
}
#endif
