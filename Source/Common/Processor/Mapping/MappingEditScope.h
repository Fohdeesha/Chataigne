/*
  ==============================================================================

	MappingEditScope.h

	A mapping's processing runs on a sequence's play thread and on the mapping's own timer thread, and both only
	try-lock the mapping : an edit that holds its lock makes them skip a frame instead of using what the edit is
	replacing. So every message-thread edit that deletes or replaces something the processing uses (an output's
	command, a command's parameters, a filter's output parameters) takes the owning mapping's lock for its whole
	duration. Anything outside a mapping (action consequences, bindings) finds none and locks nothing.

  ==============================================================================
*/

#pragma once

class MappingEditScope
{
public:
	explicit MappingEditScope(ControllableContainer* anyContainerInsideAMapping);
	~MappingEditScope();

private:
	const juce::CriticalSection* lock = nullptr;
	JUCE_DECLARE_NON_COPYABLE(MappingEditScope)
};

//A lock that may not be there
struct ScopedOptionalLock
{
	explicit ScopedOptionalLock(const juce::CriticalSection* l) : lock(l) { if (lock != nullptr) lock->enter(); }
	~ScopedOptionalLock() { if (lock != nullptr) lock->exit(); }
	const juce::CriticalSection* lock;
	JUCE_DECLARE_NON_COPYABLE(ScopedOptionalLock)
};
