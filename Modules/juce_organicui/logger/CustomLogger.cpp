
juce_ImplementSingleton(CustomLogger);

CustomLogger::CustomLogger() :
	notifier(5000, false),
	welcomeMessage(getApp().getApplicationName() + " v" + String(ProjectInfo::versionString) + " : (" + String(Time::getCompilationDate().formatted("%d/%m/%y (%R)")) + ")")
{

}

const String & CustomLogger::getWelcomeMessage() {
	return welcomeMessage;
}


void CustomLogger::logMessage(const String& message)
{
	LogElement* el = new LogElement(message);
	LogEvent* event = nullptr;
	{
		GenericScopedLock lock(logElements.getLock());
		while (logElements.size() >= MAX_LOGS) logElements.remove(0, true);
		logElements.add(el);
		event = new LogEvent(el);
	}

	notifier.addMessage(event);
	DBG(message);
}

void CustomLogger::setFileLogging(bool enabled)
{
	if(enabled && !fileWriter){
		fileWriter.reset(new CustomLogger::FileWriter());
		addLogListener(fileWriter.get());
	}
	else if(fileWriter)
	{
		removeLogListener(fileWriter.get());
		fileWriter.reset();
	}
}

CustomLogger::FileWriter::FileWriter() 
{
	fileLog.reset(FileLogger::createDateStampedLogger(getApp().getApplicationName(), "log_", ".txt", ""));
}
