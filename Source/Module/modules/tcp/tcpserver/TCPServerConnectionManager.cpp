/*
  ==============================================================================

	TCPServerConnectionManager.cpp
	Created: 4 Jul 2019 11:34:50am
	Author:  bkupe

  ==============================================================================
*/

#include "Module/ModuleIncludes.h"

TCPServerConnectionManager::TCPServerConnectionManager() :
	Thread("TCP Server Connections"),
	portToBind(0),
	queuedNotifier(10)
{
}

TCPServerConnectionManager::~TCPServerConnectionManager()
{
	close();
}

void TCPServerConnectionManager::setupReceiver(int port, const String& address)
{
	close();
	portToBind = port;
	addressToBind = address;
	startThread();
}

bool TCPServerConnectionManager::detachConnection(StreamingSocket* connection)
{
	if (connection == nullptr) return false;
	const ScopedLock sl(connections.getLock());
	if (!connections.contains(connection)) return false; //another thread took it
	connections.removeObject(connection, false);
	return true;
}

void TCPServerConnectionManager::finishConnection(StreamingSocket* connection)
{
	connectionManagerListeners.call(&ConnectionManagerListener::connectionRemoved, connection);
	queuedNotifier.addMessage(new ConnectionManagerEvent(ConnectionManagerEvent::CONNECTIONS_CHANGED));

	connection->close();
	delete connection;
}

void TCPServerConnectionManager::removeConnection(StreamingSocket* connection)
{
	if (detachConnection(connection)) finishConnection(connection);
}

void TCPServerConnectionManager::close()
{
	//The accepting thread first : closing the listener ends its wait, and a client it accepted just before was added
	//after the clients below had been removed, and kept being read. It was given 100 ms, then killed. The listener is
	//closed again while the thread is still starting : it may not have been listening yet.
	signalThreadShouldExit();
	for (int i = 0; i < 100 && isThreadRunning(); ++i)
	{
		if (receiver.isConnected()) receiver.close();
		if (waitForThreadToExit(10)) break;
	}
	stopThread(1000);

	while (StreamingSocket* s = connections.getFirst()) removeConnection(s);
}

void TCPServerConnectionManager::run()
{
	bool result = receiver.createListener(portToBind, addressToBind);
	connectionManagerListeners.call(&ConnectionManagerListener::receiverBindChanged, result);

	if (result)
	{
		while (!threadShouldExit())
		{
			StreamingSocket* socket = receiver.waitForNextConnection();
			if (socket != nullptr)
			{
				connections.add(socket);
				connectionManagerListeners.call(&ConnectionManagerListener::newConnection, socket);
				queuedNotifier.addMessage(new ConnectionManagerEvent(ConnectionManagerEvent::CONNECTIONS_CHANGED));
			}
		}
	}
	else
	{
		LOGERROR("Could not bind to port " << portToBind);
	}

	receiver.close();
}
