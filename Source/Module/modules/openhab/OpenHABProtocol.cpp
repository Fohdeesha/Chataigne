/*
  ==============================================================================

	OpenHABProtocol.cpp
	Created: 23 Sep 2026

  ==============================================================================
*/

namespace OpenHAB
{
	using namespace juce;

	static uint32 nowMs() { return Time::getMillisecondCounter(); }

	//deadline arithmetic that survives the 49.7-day wrap of the millisecond counter
	static int msUntil(uint32 deadline) { return (int)(deadline - nowMs()); }
	static bool isPast(uint32 deadline) { return msUntil(deadline) <= 0; }

	//==============================================================================
	// ChunkDecoder

	void ChunkDecoder::reset()
	{
		state = SIZE;
		remaining = 0;
		sizeDigits = 0;
		error.clear();
	}

	bool ChunkDecoder::fail(const String& message)
	{
		state = FAILED;
		error = message;
		return false;
	}

	bool ChunkDecoder::feed(const char* data, size_t size, MemoryOutputStream& out, size_t& consumed)
	{
		consumed = 0;

		while (consumed < size)
		{
			if (state == DONE) return true;
			if (state == FAILED) return false;

			const char c = data[consumed];

			switch (state)
			{
			case SIZE:
			{
				const int digit = CharacterFunctions::getHexDigitValue((juce_wchar)(uint8)c);
				if (digit >= 0)
				{
					if (sizeDigits >= 15) return fail("chunk size too large");
					remaining = remaining * 16 + (uint64)digit;
					sizeDigits++;
				}
				else if (sizeDigits == 0) return fail("chunk size expected");
				else if (c == ';' || c == ' ' || c == '\t') state = SIZE_EXTENSION;
				else if (c == '\r') state = SIZE_LF;
				else if (c == '\n') state = remaining == 0 ? TRAILER_LINE_START : DATA;
				else return fail("bad character in chunk size");
				consumed++;
			}
			break;

			case SIZE_EXTENSION:
				if (c == '\r') state = SIZE_LF;
				else if (c == '\n') state = remaining == 0 ? TRAILER_LINE_START : DATA;
				consumed++;
				break;

			case SIZE_LF:
				if (c != '\n') return fail("chunk size line not terminated");
				state = remaining == 0 ? TRAILER_LINE_START : DATA;
				consumed++;
				break;

			case DATA:
			{
				const size_t n = (size_t)jmin<uint64>(remaining, (uint64)(size - consumed));
				out.write(data + consumed, n);
				consumed += n;
				remaining -= n;
				if (remaining == 0) state = DATA_CR;
			}
			break;

			case DATA_CR:
				if (c == '\r') state = DATA_LF;
				else if (c == '\n') { state = SIZE; sizeDigits = 0; }
				else return fail("chunk not terminated");
				consumed++;
				break;

			case DATA_LF:
				if (c != '\n') return fail("chunk not terminated");
				state = SIZE;
				sizeDigits = 0;
				consumed++;
				break;

			case TRAILER_LINE_START:
				if (c == '\r') state = FINAL_LF;
				else if (c == '\n') state = DONE;
				else state = TRAILER_LINE;
				consumed++;
				break;

			case TRAILER_LINE:
				if (c == '\r') state = TRAILER_LF;
				else if (c == '\n') state = TRAILER_LINE_START;
				consumed++;
				break;

			case TRAILER_LF:
				if (c != '\n') return fail("trailer not terminated");
				state = TRAILER_LINE_START;
				consumed++;
				break;

			case FINAL_LF:
				if (c != '\n') return fail("chunked body not terminated");
				state = DONE;
				consumed++;
				break;

			default:
				return false;
			}
		}

		return state != FAILED;
	}

	//==============================================================================
	// SseParser

	void SseParser::reset()
	{
		lineSize = 0;
		skipNextLF = false;
		eventType.clear();
		dataBuffer.clear();
		hasData = false;
		lastEventId.clear();
		eventBytes = 0;
	}

	void SseParser::appendToLine(const char* data, size_t size)
	{
		if (size == 0) return;
		if (line.getSize() < lineSize + size) line.setSize(jmax<size_t>(lineSize + size, line.getSize() * 2 + 256), false);
		memcpy(static_cast<char*>(line.getData()) + lineSize, data, size);
		lineSize += size;
	}

	bool SseParser::feed(const char* data, size_t size, Array<Event>& events)
	{
		size_t runStart = 0;

		for (size_t i = 0; i < size; i++)
		{
			const char c = data[i];
			if (c != '\r' && c != '\n')
			{
				skipNextLF = false;
				continue;
			}

			appendToLine(data + runStart, i - runStart);
			runStart = i + 1;

			if (c == '\n' && skipNextLF)
			{
				skipNextLF = false;
				continue;
			}

			skipNextLF = c == '\r';

			eventBytes += lineSize;
			if (eventBytes > maxEventBytes) return false;

			processLine(events);
			lineSize = 0;
		}

		appendToLine(data + runStart, size - runStart);
		return eventBytes + lineSize <= maxEventBytes;
	}

	void SseParser::processLine(Array<Event>& events)
	{
		if (lineSize == 0)
		{
			if (hasData)
			{
				Event e;
				e.type = eventType.isEmpty() ? String("message") : eventType;
				e.data = dataBuffer;
				e.id = lastEventId;
				events.add(e);
			}

			eventType.clear();
			dataBuffer.clear();
			hasData = false;
			eventBytes = 0;
			return;
		}

		String l = String::fromUTF8(static_cast<const char*>(line.getData()), (int)lineSize);
		if (l.startsWithChar((juce_wchar)0xfeff)) l = l.substring(1);
		if (l.startsWithChar(':')) return;

		String field = l;
		String value;
		const int colon = l.indexOfChar(':');
		if (colon >= 0)
		{
			field = l.substring(0, colon);
			value = l.substring(colon + 1);
			if (value.startsWithChar(' ')) value = value.substring(1);
		}

		if (field == "event") eventType = value;
		else if (field == "data")
		{
			if (hasData) dataBuffer << "\n";
			dataBuffer << value;
			hasData = true;
		}
		else if (field == "id")
		{
			if (!value.containsChar(0)) lastEventId = value;
		}
	}

	//==============================================================================
	// Response

	String Response::getBodyAsString() const
	{
		return String::fromUTF8(static_cast<const char*>(body.getData()), (int)body.getSize());
	}

	String Response::getHeader(const String& name) const
	{
		return headers.getValue(name.toLowerCase(), String());
	}

	String Response::describe() const
	{
		if (error.isNotEmpty()) return error;
		String s = "HTTP " + String(status);
		String msg = getBodyAsString().trim();
		if (msg.startsWithChar('{'))
		{
			var d = JSON::parse(msg);
			String m = d.getProperty("error", var()).getProperty("message", "").toString();
			if (m.isNotEmpty()) msg = m;
		}
		if (msg.isNotEmpty()) s << " (" << msg.substring(0, 200) << ")";
		return s;
	}

	//==============================================================================
	// HttpConnection

	HttpConnection::HttpConnection(std::function<bool()> _shouldAbort) :
		shouldAbort(_shouldAbort)
	{
		bufferSize = 64 * 1024;
		buffer.malloc(bufferSize);
	}

	HttpConnection::~HttpConnection()
	{
		close();
	}

	void HttpConnection::setTarget(const String& _host, int _port)
	{
		if (_host == host && _port == port) return;
		close();
		host = _host;
		port = _port;
	}

	void HttpConnection::setExtraHeaders(const String& headerLines)
	{
		extraHeaders = headerLines;
	}

	void HttpConnection::close()
	{
		if (socket != nullptr) socket->close();
		socket.reset();
		bufferStart = bufferEnd = 0;
		peerClosed = false;
		requestsOnSocket = 0;
		streamMode = NO_BODY;
		streamRemaining = 0;
		streamEnded = false;
	}

	bool HttpConnection::ensureConnected(String& error)
	{
		if (socket != nullptr && socket->isConnected() && !peerClosed) return true;
		close();

		if (host.isEmpty() || port <= 0 || port > 65535)
		{
			error = "no server address";
			return false;
		}

		socket.reset(new StreamingSocket());
		if (!socket->connect(host, port, connectTimeoutMs))
		{
			socket.reset();
			error = "cannot connect to " + host + ":" + String(port);
			return false;
		}

		connectionCount++;
		lastActivity = nowMs();
		return true;
	}

	bool HttpConnection::socketLooksReusable()
	{
		if (socket == nullptr || !socket->isConnected() || peerClosed) return false;
		if (available() > 0) return false; //stray bytes from a previous response, do not guess
		if ((uint32)(nowMs() - lastActivity) > 20000) return false; //servers drop idle keep-alive connections, and a request racing that close can get lost
		return socket->waitUntilReady(true, 0) == 0; //readable while idle means the server closed it
	}

	void HttpConnection::compactBuffer()
	{
		if (bufferStart > 0)
		{
			const size_t n = available();
			if (n > 0) memmove(buffer.get(), buffer.get() + bufferStart, n);
			bufferStart = 0;
			bufferEnd = n;
		}

		if (bufferEnd == bufferSize && bufferSize < 64 * 1024 * 1024)
		{
			bufferSize *= 2;
			buffer.realloc(bufferSize);
		}
	}

	int HttpConnection::fill(int waitMs)
	{
		if (socket == nullptr || peerClosed) return -1;
		if (bufferEnd == bufferSize) compactBuffer();
		if (bufferEnd == bufferSize) return -1; //64 MB of unread data, give up rather than grow forever

		const int ready = socket->waitUntilReady(true, jmax(0, waitMs));
		if (ready < 0)
		{
			peerClosed = true;
			return -1;
		}
		if (ready == 0) return 0;

		const int n = socket->read(buffer.get() + bufferEnd, (int)(bufferSize - bufferEnd), false);
		if (n <= 0)
		{
			//readable but nothing to read is the peer closing the connection
			peerClosed = true;
			return -1;
		}

		bufferEnd += (size_t)n;
		lastActivity = nowMs();
		return n;
	}

	bool HttpConnection::writeAll(const String& data, uint32 deadline, String& error)
	{
		const char* p = data.toRawUTF8();
		size_t remaining = strlen(p);

		while (remaining > 0)
		{
			if (shouldAbort != nullptr && shouldAbort()) { error = "aborted"; return false; }
			if (isPast(deadline)) { error = "timed out sending"; return false; }

			const int ready = socket->waitUntilReady(false, 50);
			if (ready < 0) { error = "connection lost"; return false; }
			if (ready == 0) continue;

			const int w = socket->write(p, (int)jmin<size_t>(remaining, 1 << 20));
			if (w < 0) { error = "connection lost"; return false; }

			p += w;
			remaining -= (size_t)w;
		}

		lastActivity = nowMs();
		return true;
	}

	bool HttpConnection::readLine(String& lineOut, uint32 deadline, String& error)
	{
		for (;;)
		{
			const char* start = buffer.get() + bufferStart;
			const size_t n = available();
			const void* nl = n > 0 ? memchr(start, '\n', n) : nullptr;

			if (nl != nullptr)
			{
				size_t len = (size_t)(static_cast<const char*>(nl) - start);
				bufferStart += len + 1;
				if (len > 0 && start[len - 1] == '\r') len--;
				lineOut = String::fromUTF8(start, (int)len);
				return true;
			}

			if (n > 64 * 1024) { error = "header line too long"; return false; }
			if (shouldAbort != nullptr && shouldAbort()) { error = "aborted"; return false; }
			if (isPast(deadline)) { error = "timed out waiting for the server"; return false; }

			if (fill(jmin(100, jmax(0, msUntil(deadline)))) < 0)
			{
				error = "connection closed by the server";
				return false;
			}
		}
	}

	bool HttpConnection::hasToken(const String& headerValue, const String& token)
	{
		StringArray parts;
		parts.addTokens(headerValue, ",", "\"");
		for (auto& p : parts) if (p.trim().equalsIgnoreCase(token)) return true;
		return false;
	}

	bool HttpConnection::readHead(Response& r, uint32 deadline)
	{
		for (int attempt = 0; attempt < 5; attempt++) //skips 1xx interim responses and stray blank lines
		{
			String statusLine;
			if (!readLine(statusLine, deadline, r.error))
			{
				r.receivedAnyByte = r.receivedAnyByte || available() > 0 || statusLine.isNotEmpty();
				return false;
			}

			r.receivedAnyByte = true;
			if (statusLine.isEmpty()) continue;

			if (!statusLine.startsWith("HTTP/1."))
			{
				r.error = "not an HTTP response";
				return false;
			}

			r.status = statusLine.fromFirstOccurrenceOf(" ", false, false).getIntValue();
			if (r.status < 100 || r.status > 999)
			{
				r.error = "bad HTTP status line";
				return false;
			}

			r.headers.clear();
			size_t headerBytes = 0;
			for (;;)
			{
				String h;
				if (!readLine(h, deadline, r.error)) return false;
				if (h.isEmpty()) break;

				headerBytes += (size_t)h.getNumBytesAsUTF8();
				if (headerBytes > 256 * 1024)
				{
					r.error = "response headers too large";
					return false;
				}

				const int colon = h.indexOfChar(':');
				if (colon <= 0) continue;
				const String name = h.substring(0, colon).trim().toLowerCase();
				const String value = h.substring(colon + 1).trim();
				const String existing = r.headers.getValue(name, String());
				r.headers.set(name, existing.isEmpty() ? value : existing + ", " + value);
			}

			if (statusLine.startsWith("HTTP/1.0") && !hasToken(r.getHeader("connection"), "keep-alive"))
				r.headers.set("connection", "close");

			if (r.status >= 100 && r.status < 200) continue;
			return true;
		}

		r.error = "no final HTTP response";
		return false;
	}

	bool HttpConnection::readBody(Response& r, const String& method, uint32 deadline)
	{
		r.body.reset();
		if (method == "HEAD" || r.status == 204 || r.status == 304) return true;

		MemoryOutputStream out(r.body, false);
		const size_t maxBody = 256 * 1024 * 1024;

		if (hasToken(r.getHeader("transfer-encoding"), "chunked"))
		{
			ChunkDecoder decoder;
			for (;;)
			{
				if (available() > 0)
				{
					size_t consumed = 0;
					const bool ok = decoder.feed(buffer.get() + bufferStart, available(), out, consumed);
					bufferStart += consumed;
					if (!ok) { r.error = "bad chunked body : " + decoder.getError(); return false; }
					if (decoder.isDone()) break;
					if (out.getDataSize() > maxBody) { r.error = "response too large"; return false; }
				}

				if (shouldAbort != nullptr && shouldAbort()) { r.error = "aborted"; return false; }
				if (isPast(deadline)) { r.error = "timed out reading the response"; return false; }
				if (fill(jmin(100, jmax(0, msUntil(deadline)))) < 0) { r.error = "connection closed mid-response"; return false; }
			}
		}
		else if (r.getHeader("content-length").isNotEmpty())
		{
			const String lengthText = r.getHeader("content-length").upToFirstOccurrenceOf(",", false, false).trim();
			if (!lengthText.containsOnly("0123456789") || lengthText.isEmpty() || lengthText.length() > 15) { r.error = "bad Content-Length"; return false; }
			uint64 remaining = (uint64)lengthText.getLargeIntValue();
			if (remaining > maxBody) { r.error = "response too large"; return false; }

			while (remaining > 0)
			{
				if (available() > 0)
				{
					const size_t n = (size_t)jmin<uint64>(remaining, (uint64)available());
					out.write(buffer.get() + bufferStart, n);
					bufferStart += n;
					remaining -= n;
					continue;
				}

				if (shouldAbort != nullptr && shouldAbort()) { r.error = "aborted"; return false; }
				if (isPast(deadline)) { r.error = "timed out reading the response"; return false; }
				if (fill(jmin(100, jmax(0, msUntil(deadline)))) < 0) { r.error = "connection closed mid-response"; return false; }
			}
		}
		else
		{
			//no framing : the body runs until the server closes the connection
			for (;;)
			{
				if (available() > 0)
				{
					out.write(buffer.get() + bufferStart, available());
					bufferStart = bufferEnd;
					if (out.getDataSize() > maxBody) { r.error = "response too large"; return false; }
				}

				if (shouldAbort != nullptr && shouldAbort()) { r.error = "aborted"; return false; }
				if (isPast(deadline)) { r.error = "timed out reading the response"; return false; }
				if (fill(jmin(100, jmax(0, msUntil(deadline)))) < 0) break;
			}

			r.headers.set("connection", "close");
		}

		return true;
	}

	String HttpConnection::buildRequest(const String& method, const String& target, const String& accept, const String& body, const String& contentType) const
	{
		String hostHeader = host.containsChar(':') && !host.startsWithChar('[') ? "[" + host + "]" : host;
		String req;
		req << method << " " << target << " HTTP/1.1\r\n";
		req << "Host: " << hostHeader << ":" << port << "\r\n";
		req << "User-Agent: Chataigne\r\n";
		req << "Accept: " << (accept.isNotEmpty() ? accept : String("application/json")) << "\r\n";
		req << "Connection: keep-alive\r\n";
		req << extraHeaders;

		if (method == "POST" || method == "PUT" || body.isNotEmpty())
		{
			if (contentType.isNotEmpty()) req << "Content-Type: " << contentType << "\r\n";
			req << "Content-Length: " << (int)body.getNumBytesAsUTF8() << "\r\n";
		}

		req << "\r\n" << body;
		return req;
	}

	Response HttpConnection::request(const String& method, const String& target, const String& body, const String& contentType, int timeoutMs, bool retryOnStaleConnection)
	{
		Response r;
		const uint32 deadline = nowMs() + (uint32)jmax(100, timeoutMs);

		if (streamMode != NO_BODY) close(); //a streaming response owns the socket until it ends

		bool reused = socket != nullptr && requestsOnSocket > 0;
		if (reused && !socketLooksReusable())
		{
			close();
			reused = false;
		}

		if (!ensureConnected(r.error)) return r;

		if (!writeAll(buildRequest(method, target, String(), body, contentType), deadline, r.error))
		{
			close();
			if (reused && retryOnStaleConnection) return request(method, target, body, contentType, timeoutMs, false);
			return r;
		}
		r.requestWritten = true;

		if (!readHead(r, deadline))
		{
			const bool nothingCameBack = !r.receivedAnyByte;
			close();
			//a keep-alive connection the server closed as we wrote to it : the request never reached it, send it again once
			if (reused && nothingCameBack && retryOnStaleConnection && !(shouldAbort != nullptr && shouldAbort()) && !isPast(deadline))
				return request(method, target, body, contentType, jmax(100, msUntil(deadline)), false);
			return r;
		}

		if (!readBody(r, method, deadline))
		{
			close();
			return r;
		}

		requestsOnSocket++;
		if (hasToken(r.getHeader("connection"), "close")) close();
		return r;
	}

	bool HttpConnection::openStream(const String& target, const String& accept, int timeoutMs, Response& head)
	{
		close(); //a stream always gets its own fresh connection
		const uint32 deadline = nowMs() + (uint32)jmax(100, timeoutMs);

		if (!ensureConnected(head.error)) return false;

		if (!writeAll(buildRequest("GET", target, accept, String(), String()), deadline, head.error) || !readHead(head, deadline))
		{
			close();
			return false;
		}

		requestsOnSocket++;
		streamEnded = false;
		streamDecoder.reset();

		if (hasToken(head.getHeader("transfer-encoding"), "chunked")) streamMode = CHUNKED;
		else if (head.getHeader("content-length").isNotEmpty())
		{
			streamMode = CONTENT_LENGTH;
			streamRemaining = (uint64)head.getHeader("content-length").getLargeIntValue();
		}
		else streamMode = UNTIL_CLOSE;

		return true;
	}

	int HttpConnection::readStream(MemoryOutputStream& out, int waitMs)
	{
		if (streamEnded || socket == nullptr) return -1;

		if (available() == 0)
		{
			const int f = fill(waitMs);
			if (f < 0)
			{
				streamEnded = true;
				return -1;
			}
			if (f == 0) return 0;
		}

		const size_t before = out.getDataSize();

		switch (streamMode)
		{
		case CHUNKED:
		{
			size_t consumed = 0;
			const bool ok = streamDecoder.feed(buffer.get() + bufferStart, available(), out, consumed);
			bufferStart += consumed;
			if (!ok || streamDecoder.isDone()) streamEnded = true;
		}
		break;

		case CONTENT_LENGTH:
		{
			const size_t n = (size_t)jmin<uint64>(streamRemaining, (uint64)available());
			out.write(buffer.get() + bufferStart, n);
			bufferStart += n;
			streamRemaining -= n;
			if (streamRemaining == 0) streamEnded = true;
		}
		break;

		case UNTIL_CLOSE:
			out.write(buffer.get() + bufferStart, available());
			bufferStart = bufferEnd;
			break;

		default:
			streamEnded = true;
			break;
		}

		const int appended = (int)(out.getDataSize() - before);
		if (streamEnded && appended == 0) return -1;
		return appended;
	}

	//==============================================================================
	// Items

	bool isValidItemName(const String& name)
	{
		if (name.isEmpty() || name.length() > 256) return false;
		return name.containsOnly("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_");
	}

	static bool readNumberProperty(const var& o, const Identifier& id, double& out)
	{
		if (!o.hasProperty(id)) return false;
		const var v = o.getProperty(id, var());
		if (v.isInt() || v.isInt64() || v.isDouble())
		{
			out = (double)v;
			return std::isfinite(out);
		}
		if (v.isString()) return parseNumber(v.toString(), out) && std::isfinite(out);
		return false;
	}

	Kind kindForBaseType(const String& baseType, bool hasOptions)
	{
		if (baseType == "Switch") return Kind::SWITCH;
		if (baseType == "Contact") return Kind::CONTACT;
		if (baseType == "Dimmer") return Kind::DIMMER;
		if (baseType == "Rollershutter") return Kind::ROLLERSHUTTER;
		if (baseType == "Number") return hasOptions ? Kind::OPTIONS : Kind::NUMBER;
		if (baseType == "Color") return Kind::COLOR;
		if (baseType == "String") return hasOptions ? Kind::OPTIONS : Kind::STRING;
		if (baseType == "Player") return Kind::PLAYER;
		if (baseType == "DateTime") return Kind::DATETIME;
		if (baseType == "Location") return Kind::LOCATION;
		if (baseType == "Call") return Kind::CALL;
		return Kind::NONE;
	}

	String kindName(Kind k)
	{
		switch (k)
		{
		case Kind::SWITCH: return "Switch";
		case Kind::CONTACT: return "Contact";
		case Kind::DIMMER: return "Dimmer";
		case Kind::ROLLERSHUTTER: return "Rollershutter";
		case Kind::NUMBER: return "Number";
		case Kind::COLOR: return "Color";
		case Kind::STRING: return "String";
		case Kind::OPTIONS: return "Options";
		case Kind::PLAYER: return "Player";
		case Kind::DATETIME: return "DateTime";
		case Kind::LOCATION: return "Location";
		case Kind::CALL: return "Call";
		default: return "None";
		}
	}

	bool kindIsReadOnly(Kind k)
	{
		return k == Kind::CONTACT || k == Kind::CALL;
	}

	bool parseItem(const var& dto, ItemSpec& spec)
	{
		if (!dto.isObject()) return false;

		spec = ItemSpec();
		spec.name = dto.getProperty("name", "").toString();
		if (!isValidItemName(spec.name)) return false;

		spec.type = dto.getProperty("type", "").toString().trim();
		if (spec.type.isEmpty()) return false;

		spec.label = dto.getProperty("label", "").toString().trim();
		spec.isGroup = spec.type == "Group" || spec.type.startsWith("Group:");

		String base = spec.isGroup ? dto.getProperty("groupType", "").toString().trim() : spec.type;
		if (spec.isGroup && base.isEmpty() && spec.type.startsWith("Group:")) base = spec.type.fromFirstOccurrenceOf(":", false, false);

		if (base.containsChar(':'))
		{
			spec.dimension = base.fromFirstOccurrenceOf(":", false, false);
			base = base.upToFirstOccurrenceOf(":", false, false);
		}
		spec.baseType = base;

		const var sd = dto.getProperty("stateDescription", var());
		if (sd.isObject())
		{
			spec.readOnly = (bool)sd.getProperty("readOnly", false);
			spec.pattern = sd.getProperty("pattern", "").toString();
			spec.hasMinimum = readNumberProperty(sd, "minimum", spec.minimum);
			spec.hasMaximum = readNumberProperty(sd, "maximum", spec.maximum);
			double step = 0;
			if (readNumberProperty(sd, "step", step) && step > 0) spec.step = step;

			if (const Array<var>* opts = sd.getProperty("options", var()).getArray())
			{
				for (auto& o : *opts)
				{
					const String v = o.getProperty("value", "").toString();
					if (v.isEmpty()) continue;
					spec.options.add({ v, o.getProperty("label", "").toString().trim() });
				}
			}
		}

		const var cd = dto.getProperty("commandDescription", var());
		if (cd.isObject())
		{
			if (const Array<var>* opts = cd.getProperty("commandOptions", var()).getArray())
			{
				for (auto& o : *opts)
				{
					const String v = o.getProperty("command", "").toString();
					if (v.isEmpty()) continue;
					bool exists = false;
					for (auto& e : spec.options) if (e.value == v) { exists = true; break; }
					if (!exists) spec.options.add({ v, o.getProperty("label", "").toString().trim() });
				}
			}
		}

		if (dto.hasProperty("state"))
		{
			spec.state = dto.getProperty("state", "").toString();
			spec.hasState = true;
		}

		spec.kind = kindForBaseType(spec.baseType, !spec.options.isEmpty());

		if (spec.kind == Kind::NONE)
		{
			if (spec.isGroup && spec.baseType.isEmpty()) spec.skipReason = "group without a type";
			else if (spec.baseType == "Image") spec.skipReason = "Image";
			else spec.skipReason = "unsupported type " + spec.type;
		}

		if (kindIsReadOnly(spec.kind)) spec.readOnly = true;
		return true;
	}

	//==============================================================================
	// States

	bool isUndefinedState(const String& stateType, const String& value)
	{
		if (stateType.isNotEmpty()) return stateType == "UnDef";
		return value == "NULL" || value == "UNDEF";
	}

	bool parseNumber(const String& text, double& value, String* unit)
	{
		const String t = text.trim();
		int i = 0;
		const int len = t.length();

		auto isDigit = [](juce_wchar c) { return c >= '0' && c <= '9'; };

		if (i < len && (t[i] == '+' || t[i] == '-')) i++;
		int digits = 0;
		while (i < len && isDigit(t[i])) { i++; digits++; }
		if (i < len && t[i] == '.')
		{
			i++;
			while (i < len && isDigit(t[i])) { i++; digits++; }
		}
		if (digits == 0) return false;

		if (i < len && (t[i] == 'e' || t[i] == 'E'))
		{
			int j = i + 1;
			if (j < len && (t[j] == '+' || t[j] == '-')) j++;
			int expDigits = 0;
			while (j < len && isDigit(t[j])) { j++; expDigits++; }
			if (expDigits > 0) i = j;
		}

		const String rest = t.substring(i);
		if (rest.isNotEmpty() && !CharacterFunctions::isWhitespace(rest[0]) && rest[0] != '%' && rest[0] != (juce_wchar)0xb0 && !CharacterFunctions::isLetter(rest[0]))
			return false;

		value = t.substring(0, i).getDoubleValue();
		if (!std::isfinite(value)) return false;
		if (unit != nullptr) *unit = rest.trim();
		return true;
	}

	bool parseHSB(const String& text, double& hue, double& saturation, double& brightness)
	{
		StringArray parts;
		parts.addTokens(text, ",", "");
		if (parts.size() != 3) return false;

		double v[3];
		for (int i = 0; i < 3; i++)
		{
			String unit;
			if (!parseNumber(parts[i], v[i], &unit) || unit.isNotEmpty()) return false;
		}

		if (v[0] < 0 || v[0] > 360 || v[1] < 0 || v[1] > 100 || v[2] < 0 || v[2] > 100) return false;
		hue = v[0];
		saturation = v[1];
		brightness = v[2];
		return true;
	}

	void hsbToRgb(double hue, double saturation, double brightness, double& r, double& g, double& b)
	{
		const double s = jlimit(0.0, 1.0, saturation);
		const double v = jlimit(0.0, 1.0, brightness);
		double h = std::fmod(hue, 360.0);
		if (h < 0) h += 360.0;

		const double c = v * s;
		const double hp = h / 60.0;
		const double x = c * (1.0 - std::abs(std::fmod(hp, 2.0) - 1.0));
		double r1 = 0, g1 = 0, b1 = 0;

		if (hp < 1) { r1 = c; g1 = x; }
		else if (hp < 2) { r1 = x; g1 = c; }
		else if (hp < 3) { g1 = c; b1 = x; }
		else if (hp < 4) { g1 = x; b1 = c; }
		else if (hp < 5) { r1 = x; b1 = c; }
		else { r1 = c; b1 = x; }

		const double m = v - c;
		r = r1 + m;
		g = g1 + m;
		b = b1 + m;
	}

	void rgbToHsb(double r, double g, double b, double& hue, double& saturation, double& brightness)
	{
		r = jlimit(0.0, 1.0, r);
		g = jlimit(0.0, 1.0, g);
		b = jlimit(0.0, 1.0, b);

		const double maxC = jmax(r, g, b);
		const double minC = jmin(r, g, b);
		const double delta = maxC - minC;

		brightness = maxC;
		saturation = maxC <= 0 ? 0 : delta / maxC;

		if (delta <= 0) hue = 0;
		else if (maxC == r) hue = 60.0 * std::fmod((g - b) / delta, 6.0);
		else if (maxC == g) hue = 60.0 * ((b - r) / delta + 2.0);
		else hue = 60.0 * ((r - g) / delta + 4.0);

		if (hue < 0) hue += 360.0;
		if (hue >= 360.0) hue -= 360.0;
	}

	static String formatDecimal(double v, int maxDecimals)
	{
		if (!std::isfinite(v)) return String();

		//String(double, 0) falls back to the stream's general format, which writes 1234567 as 1.23457e+06
		if (maxDecimals <= 0 || std::abs(v) >= 1e15)
		{
			if (std::abs(v) < 9e15) return String((int64)std::llround(v));
			return String(v, 1, false).upToFirstOccurrenceOf(".", false, false);
		}

		String s = String(v, maxDecimals, false);
		if (s.containsChar('.'))
		{
			s = s.trimCharactersAtEnd("0");
			if (s.endsWithChar('.')) s = s.dropLastCharacters(1);
		}
		if (s == "-0") s = "0";
		return s;
	}

	static int decimalsForSignificantDigits(double v, int digits)
	{
		if (v == 0) return 0;
		const int exponent = (int)std::floor(std::log10(std::abs(v)));
		return jlimit(0, 15, digits - 1 - exponent);
	}

	String formatNumber(double value, double step)
	{
		if (!std::isfinite(value)) return String();

		if (step > 0)
		{
			value = std::round(value / step) * step;
			int decimals = 0;
			double st = step;
			while (decimals < 10 && std::abs(st - std::round(st)) > 1e-9) { st *= 10; decimals++; }
			return formatDecimal(value, decimals);
		}

		//a value that is exactly a float came from a float source (OSC, a slider, an automation) : write the
		//shortest decimal that is the same float, so 33.3f goes out as 33.3 and not 33.299999
		const float f = (float)value;
		if ((double)f == value)
		{
			for (int digits = 1; digits <= 9; digits++)
			{
				const String s = formatDecimal(value, decimalsForSignificantDigits(value, digits));
				if ((float)s.getDoubleValue() == f) return s;
			}
		}

		return formatDecimal(value, decimalsForSignificantDigits(value, 15));
	}

	String formatHSB(double hue, double saturation, double brightness)
	{
		double h = std::fmod(hue, 360.0);
		if (h < 0) h += 360.0;
		String hs = formatDecimal(h, 2);
		if (hs == "360") hs = "0";
		return hs + "," + formatDecimal(jlimit(0.0, 100.0, saturation), 2) + "," + formatDecimal(jlimit(0.0, 100.0, brightness), 2);
	}

	bool sameNumber(double a, double b, double step)
	{
		if (step > 0) return formatNumber(a, step) == formatNumber(b, step);
		return std::abs(a - b) <= 1e-6 * jmax(1.0, std::abs(a), std::abs(b));
	}

	bool sameHSB(const String& a, const String& b, double tolerance)
	{
		double h1, s1, b1, h2, s2, b2;
		if (!parseHSB(a, h1, s1, b1) || !parseHSB(b, h2, s2, b2)) return a == b;

		double dh = std::abs(h1 - h2);
		if (dh > 180) dh = 360 - dh;

		//hue and saturation mean nothing on a black light, and hue nothing on a grey one
		if (b1 <= tolerance && b2 <= tolerance) return true;
		if (s1 <= tolerance && s2 <= tolerance) return std::abs(b1 - b2) <= tolerance;
		return dh <= tolerance && std::abs(s1 - s2) <= tolerance && std::abs(b1 - b2) <= tolerance;
	}

	//==============================================================================
	// Filter

	void ItemFilter::set(const String& text)
	{
		includes.clear();
		excludes.clear();

		StringArray parts;
		parts.addTokens(text, ",;", "");
		for (auto p : parts)
		{
			p = p.trim();
			if (p.isEmpty()) continue;
			if (p.startsWithChar('!'))
			{
				p = p.substring(1).trim();
				if (p.isNotEmpty()) excludes.add(p);
			}
			else includes.add(p);
		}
	}

	bool ItemFilter::matches(const String& itemName) const
	{
		if (!includes.isEmpty())
		{
			bool found = false;
			for (auto& p : includes) if (itemName.matchesWildcard(p, true)) { found = true; break; }
			if (!found) return false;
		}

		for (auto& p : excludes) if (itemName.matchesWildcard(p, true)) return false;
		return true;
	}

	//==============================================================================

	bool parseItemTopic(const String& topic, TopicParts& parts)
	{
		StringArray t;
		t.addTokens(topic, "/", "");
		if (t.size() < 4 || t[0] != "openhab" || t[1] != "items") return false;

		parts = TopicParts();
		parts.itemName = t[2];
		if (t.size() == 4) parts.action = t[3];
		else if (t.size() == 5)
		{
			parts.memberName = t[3];
			parts.action = t[4];
		}
		else return false;

		return isValidItemName(parts.itemName);
	}

	String sanitizeHost(const String& text, int& portOverride, bool& wasHttps)
	{
		portOverride = 0;
		wasHttps = false;

		String h = text.trim();
		if (h.startsWithIgnoreCase("https://"))
		{
			wasHttps = true;
			h = h.substring(8);
		}
		else if (h.startsWithIgnoreCase("http://")) h = h.substring(7);

		h = h.upToFirstOccurrenceOf("/", false, false);
		h = h.upToFirstOccurrenceOf("?", false, false);
		h = h.fromLastOccurrenceOf("@", false, false); //user:password@ belongs in the authentication fields

		if (h.startsWithChar('['))
		{
			const String inside = h.substring(1).upToFirstOccurrenceOf("]", false, false);
			const String after = h.fromFirstOccurrenceOf("]", false, false);
			if (after.startsWithChar(':')) portOverride = after.substring(1).getIntValue();
			h = inside;
		}
		else if (h.containsChar(':') && h.indexOfChar(':') == h.lastIndexOfChar(':'))
		{
			const String p = h.fromFirstOccurrenceOf(":", false, false);
			if (p.containsOnly("0123456789") && p.isNotEmpty()) portOverride = p.getIntValue();
			h = h.upToFirstOccurrenceOf(":", false, false);
		}

		if (portOverride < 1 || portOverride > 65535) portOverride = 0;

		//"localhost" resolves to ::1 first on Windows, and a refused IPv6 attempt costs the whole connect timeout
		//before 127.0.0.1 is tried, when openHAB only listens on IPv4
		if (h.trim().equalsIgnoreCase("localhost")) return "127.0.0.1";
		return h.trim();
	}
}
