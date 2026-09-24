/*
  ==============================================================================

    TCPServerConnectionManager.h
    Created: 4 Jul 2019 11:34:50am
    Author:  bkupe

  ==============================================================================
*/

#pragma once

class TCPServerConnectionManager :
	public Thread
{
public:
	TCPServerConnectionManager();
	~TCPServerConnectionManager();

	StreamingSocket receiver;
	OwnedArray<StreamingSocket, CriticalSection> connections; 
	int portToBind;
	String addressToBind;

	void setupReceiver(int port, const String& address);

	//The reading thread and a sending thread (message thread, a sequence's play thread, a script) can find the same
	//client dead, and both removed and deleted it. A client is now taken out of the list under the list's lock, by one
	//of them only (detachConnection returns false to the others), then closed and deleted by that one.
	bool detachConnection(StreamingSocket* connection);
	void finishConnection(StreamingSocket* connection);
	void removeConnection(StreamingSocket* connection);

	void close();

	class ConnectionManagerListener
	{
	public:
		virtual ~ConnectionManagerListener() {}
		virtual void receiverBindChanged(bool /*isBound*/) {}
		virtual void newConnection(StreamingSocket*) {}
		virtual void connectionRemoved(StreamingSocket *) {}
	};

	//Locked : the accepting thread (a new client), the reading thread and any sending thread (a client removed) call it
	//at once, and an unlocked ListenerList keeps its iteration state in a shared vector : it was corrupted, and the
	//destructor then crashed walking it. No callback waits for another thread, so holding it through a call is safe.
	ListenerList<ConnectionManagerListener, Array<ConnectionManagerListener*, CriticalSection>> connectionManagerListeners;
	void addConnectionManagerListener(ConnectionManagerListener* newListener) { connectionManagerListeners.add(newListener); }
	void removeConnectionManagerListener(ConnectionManagerListener* listener) { connectionManagerListeners.remove(listener); }

	class  ConnectionManagerEvent
	{
	public:
		enum Type { CONNECTIONS_CHANGED };

		ConnectionManagerEvent(Type t) : type(t) {}

		Type type;
	};

	QueuedNotifier<ConnectionManagerEvent> queuedNotifier;
	typedef QueuedNotifier<ConnectionManagerEvent>::Listener AsyncListener;


	void addAsyncConnectionManagerListener(AsyncListener* newListener) { queuedNotifier.addListener(newListener); }
	void addAsyncCoalescedConnectionManagerListener(AsyncListener* newListener) { queuedNotifier.addAsyncCoalescedListener(newListener); }
	void removeAsyncConnectionManagerListener(AsyncListener* listener) { queuedNotifier.removeListener(listener); }



	// Inherited via Thread
	virtual void run() override;

};