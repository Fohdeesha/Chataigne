/*
  ==============================================================================

    DashboardManager.h
    Created: 19 Apr 2017 10:57:53pm
    Author:  Ben

  ==============================================================================
*/

#pragma once

#if ORGANICUI_USE_WEBSERVER
#include <juce_simpleweb/juce_simpleweb.h>
#endif


#if ORGANICUI_USE_SERVUS
#include "servus/servus.h"

class ServusThread :
	public juce::Thread
{
public:
	ServusThread();
	servus::Servus servus;
	void setupZeroconf();
	void run() override;
};
#endif


class DashboardManager :
	public BaseManager<Dashboard>,
	public Dashboard::DashboardListener,
	public EngineListener
#if ORGANICUI_USE_WEBSERVER
	,public SimpleWebSocketServer::RequestHandler
	,public SimpleWebSocketServer::Listener
	,public juce::URL::DownloadTask::Listener
#endif
{
public:
	juce_DeclareSingleton(DashboardManager, true);

	DashboardManager();
	~DashboardManager();

	BoolParameter* editMode;
	BoolParameter* snapping;
	IntParameter* connectedClients;

	ColorParameter* tabsBGColor;
	ColorParameter* tabsLabelColor;
	ColorParameter* tabsBorderColor;
	FloatParameter * tabsBorderWidth;
	ColorParameter* tabsSelectedBGColor;
	ColorParameter* tabsSelectedLabelColor;
	ColorParameter* tabsSelectedBorderColor;
	FloatParameter* tabsSelectedBorderWidth;

	CommentManager commentManager;


	void addItemInternal(Dashboard* item, juce::var data) override;
	void removeItemInternal(Dashboard* item) override;

	void setCurrentDashboard(Dashboard* d, bool setInClients = false, bool setInNative = false, juce::StringArray excludeIds = juce::StringArray());

	void parameterFeedback(juce::var data) override;
	void dashboardFeedback(juce::var data) override;
	void askForRefresh(Dashboard *d) override;

	void fileLoaded() override;

#if ORGANICUI_USE_WEBSERVER
	std::unique_ptr<SimpleWebSocketServerBase> server;

	juce::String downloadURL;
	juce::File serverRootPath;
	juce::File downloadedFileZip;
	std::unique_ptr<juce::URL::DownloadTask> downloadTask;

	std::function<juce::String(std::shared_ptr<HttpServer::Response> response, std::shared_ptr<HttpServer::Request> request, SimpleWeb::CaseInsensitiveMultimap &header)> customHandleHTTPRequestFunc;
	void setupDownloadURL(const juce::String& downloadURL);

	void downloadDashboardFiles();
	void progress(juce::URL::DownloadTask* task, juce::int64 bytesDownloaded, juce::int64 bytesTotal) override;
	void finished(juce::URL::DownloadTask* task, bool success) override;

	void setupServer();
	void connectionOpened(const juce::String& id) override;
	void messageReceived(const juce::String& id, const juce::String& message) override;
	void connectionClosed(const juce::String& id, int status, const juce::String& reason) override;
	void connectionError(const juce::String& id, int status, const juce::String& errorMessage) override;

	juce::var getServerData();

	//The server's thread only parses : everything that reads or changes the tree runs on the message thread (a load, an
	//undo or a delete frees objects under the server's thread otherwise)
	juce::String buildServerDataString();
	void handleWebMessage(const juce::String& id, juce::var data);

	//What a web client may reach : only what the dashboards show. Any LAN host could otherwise set any parameter of the
	//session and download any file a File parameter points at (a script, a show file) by its address.
	enum WebAccess { WEB_NONE, WEB_READ, WEB_WRITE };
	WebAccess getWebAccess(Controllable* c);
	juce::uint32 lastRefusedWebLog = 0;
	std::atomic<int> pendingWebMessages{ 0 };

	// Inherited via RequestHandler
	virtual bool handleHTTPRequest(std::shared_ptr<HttpServer::Response> response, std::shared_ptr<HttpServer::Request> request) override;

#if SIMPLEWEB_SECURE_SUPPORTED
	virtual bool handleHTTPSRequest(std::shared_ptr<HttpsServer::Response> response, std::shared_ptr<HttpsServer::Request> request) override;
#endif

#endif


private:
#if ORGANICUI_USE_SERVUS
	ServusThread servusThread;
#endif


	
};

