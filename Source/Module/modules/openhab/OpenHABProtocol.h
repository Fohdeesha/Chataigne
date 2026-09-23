/*
  ==============================================================================

	OpenHABProtocol.h
	Created: 23 Sep 2026

	Transport and data conversion for the openHAB module. Depends on juce_core only.

  ==============================================================================
*/

#pragma once

namespace OpenHAB
{
	//Decodes an HTTP/1.1 chunked body incrementally, whatever the byte boundaries.
	class ChunkDecoder
	{
	public:
		void reset();
		//Stops at the end of the body, so bytes of a following response are never consumed. False on a protocol error.
		bool feed(const char* data, size_t size, juce::MemoryOutputStream& out, size_t& consumed);
		bool isDone() const { return state == DONE; }
		juce::String getError() const { return error; }

	private:
		enum State { SIZE, SIZE_EXTENSION, SIZE_LF, DATA, DATA_CR, DATA_LF, TRAILER_LINE_START, TRAILER_LINE, TRAILER_LF, FINAL_LF, DONE, FAILED };
		State state = SIZE;
		juce::uint64 remaining = 0;
		int sizeDigits = 0;
		juce::String error;

		bool fail(const juce::String& message);
	};

	//Server-sent events, as specified by the WHATWG EventSource algorithm.
	class SseParser
	{
	public:
		struct Event
		{
			juce::String type;
			juce::String data;
			juce::String id;
		};

		void reset();
		bool feed(const char* data, size_t size, juce::Array<Event>& events); //false if an event grew past maxEventBytes

		size_t maxEventBytes = 32 * 1024 * 1024;

	private:
		juce::MemoryBlock line;
		size_t lineSize = 0;
		bool skipNextLF = false;
		juce::String eventType;
		juce::String dataBuffer;
		bool hasData = false;
		juce::String lastEventId;
		size_t eventBytes = 0;

		void appendToLine(const char* data, size_t size);
		void processLine(juce::Array<Event>& events);
	};

	struct Response
	{
		int status = 0;
		juce::StringPairArray headers; //names lower-cased
		juce::MemoryBlock body;
		juce::String error; //transport failure, empty when a response was read
		bool receivedAnyByte = false;

		bool succeeded() const { return error.isEmpty() && status >= 200 && status < 300; }
		juce::String getBodyAsString() const;
		juce::String getHeader(const juce::String& name) const;
		juce::String describe() const;
	};

	//One keep-alive HTTP/1.1 connection over a plain socket. Not thread safe : each thread owns its own.
	//Every blocking step waits in short slices and gives up as soon as shouldAbort() returns true.
	class HttpConnection
	{
	public:
		HttpConnection(std::function<bool()> shouldAbort);
		~HttpConnection();

		void setTarget(const juce::String& host, int port);
		void setExtraHeaders(const juce::String& headerLines); //each line terminated by \r\n
		void setConnectTimeout(int ms) { connectTimeoutMs = ms; }

		Response request(const juce::String& method, const juce::String& target, const juce::String& body = juce::String(), const juce::String& contentType = juce::String(), int timeoutMs = 5000, bool retryOnStaleConnection = true);

		//Sends a GET and reads the response head. The body is then read with readStream().
		bool openStream(const juce::String& target, const juce::String& accept, int timeoutMs, Response& head);
		//Waits up to waitMs for body bytes and appends them, decoded. Returns the number of bytes appended, or -1 once the stream has ended or failed.
		int readStream(juce::MemoryOutputStream& out, int waitMs);

		void close();
		bool isOpen() const { return socket != nullptr && socket->isConnected(); }
		juce::uint32 getLastActivityTime() const { return lastActivity; }
		int getRequestsOnSocket() const { return requestsOnSocket; }
		int getConnectionCount() const { return connectionCount; }

		juce::String host;
		int port = 0;

	private:
		std::function<bool()> shouldAbort;
		std::unique_ptr<juce::StreamingSocket> socket;
		juce::String extraHeaders;
		int connectTimeoutMs = 1500;

		juce::HeapBlock<char> buffer;
		size_t bufferSize = 0;
		size_t bufferStart = 0;
		size_t bufferEnd = 0;
		bool peerClosed = false;

		juce::uint32 lastActivity = 0;
		int requestsOnSocket = 0;
		int connectionCount = 0;

		enum BodyMode { NO_BODY, CONTENT_LENGTH, CHUNKED, UNTIL_CLOSE };
		BodyMode streamMode = NO_BODY;
		juce::uint64 streamRemaining = 0;
		ChunkDecoder streamDecoder;
		bool streamEnded = false;

		bool ensureConnected(juce::String& error);
		bool socketLooksReusable();
		bool writeAll(const juce::String& data, juce::uint32 deadline, juce::String& error);
		int fill(int waitMs); //>0 bytes read, 0 nothing yet, -1 closed or failed
		bool readLine(juce::String& lineOut, juce::uint32 deadline, juce::String& error);
		bool readHead(Response& r, juce::uint32 deadline);
		bool readBody(Response& r, const juce::String& method, juce::uint32 deadline);
		size_t available() const { return bufferEnd - bufferStart; }
		void compactBuffer();
		juce::String buildRequest(const juce::String& method, const juce::String& target, const juce::String& accept, const juce::String& body, const juce::String& contentType) const;
		static bool hasToken(const juce::String& headerValue, const juce::String& token);
	};

	//What an openHAB item becomes in Chataigne.
	enum class Kind { NONE, SWITCH, CONTACT, DIMMER, ROLLERSHUTTER, NUMBER, COLOR, STRING, OPTIONS, PLAYER, DATETIME, LOCATION, CALL };

	struct Option
	{
		juce::String value;
		juce::String label;
	};

	struct ItemSpec
	{
		juce::String name;
		juce::String type; //as openHAB reports it, e.g. "Number:Temperature" or "Group"
		juce::String baseType; //the type that decides the parameter : groupType for a group, dimension removed
		juce::String dimension;
		juce::String label;
		juce::String pattern;
		bool isGroup = false;
		bool readOnly = false;
		bool hasMinimum = false;
		bool hasMaximum = false;
		double minimum = 0;
		double maximum = 0;
		double step = 0;
		juce::Array<Option> options;
		Kind kind = Kind::NONE;
		juce::String skipReason;
		juce::String state; //present when the server sent it along
		bool hasState = false;
	};

	bool isValidItemName(const juce::String& name);
	bool parseItem(const juce::var& dto, ItemSpec& spec); //false when the entry is not a usable item description
	Kind kindForBaseType(const juce::String& baseType, bool hasOptions);
	juce::String kindName(Kind k);
	bool kindIsReadOnly(Kind k);

	bool isUndefinedState(const juce::String& stateType, const juce::String& value);
	bool parseNumber(const juce::String& text, double& value, juce::String* unit = nullptr);
	bool parseHSB(const juce::String& text, double& hue, double& saturation, double& brightness);
	void hsbToRgb(double hue, double saturation, double brightness, double& r, double& g, double& b); //hue 0-360, others 0-1
	void rgbToHsb(double r, double g, double b, double& hue, double& saturation, double& brightness);
	juce::String formatNumber(double value, double step = 0);
	juce::String formatHSB(double hue, double saturation, double brightness); //hue 0-360, saturation and brightness 0-100
	bool sameNumber(double a, double b, double step = 0);
	bool sameHSB(const juce::String& a, const juce::String& b, double tolerance = 0.05); //degrees and percentage points

	//Comma separated name patterns, * and ? as wildcards, a leading ! excludes. Empty = everything.
	class ItemFilter
	{
	public:
		void set(const juce::String& text);
		bool matches(const juce::String& itemName) const;
		bool isEmpty() const { return includes.isEmpty() && excludes.isEmpty(); }

	private:
		juce::StringArray includes;
		juce::StringArray excludes;
	};

	struct TopicParts
	{
		juce::String itemName;
		juce::String memberName;
		juce::String action; //statechanged, stateupdated, added, removed, updated, command...
	};
	bool parseItemTopic(const juce::String& topic, TopicParts& parts);

	juce::String sanitizeHost(const juce::String& text, int& portOverride, bool& wasHttps);
}
