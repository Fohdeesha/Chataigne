/*
  ==============================================================================

    MappingOutputManager.cpp
    Created: 28 Oct 2016 8:11:54pm
    Author:  bkupe

  ==============================================================================
*/

#include "Common/Processor/ProcessorIncludes.h"
#include "Common/Processor/Mapping/MappingEditScope.h"

MappingOutputManager::MappingOutputManager(Multiplex * multiplex) :
	BaseManager<MappingOutput>("Outputs"),
	MultiplexTarget(multiplex),
	forceDisabled(false),
	omAsyncNotifier(5)
{
	canBeCopiedAndPasted = true;
	selectItemWhenCreated = false;
}

MappingOutputManager::~MappingOutputManager()
{
}


void MappingOutputManager::clear()
{
	for (auto& o : items) o->removeCommandHandlerListener(this);
	BaseManager::clear();
}

MappingOutput* MappingOutputManager::createItem()
{
	MappingOutput * o = new MappingOutput(multiplex);
	o->setForceDisabled(forceDisabled);
	return o;
}

void MappingOutputManager::setForceDisabled(bool value)
{
	forceDisabled = value;
	for (auto& i : items) i->forceDisabled = value;
}

void MappingOutputManager::setOutParams(Array<Parameter *> params, int multiplexIndex)
{
	outParams.ensureStorageAllocated(multiplexIndex + 1);
	outParams.set(multiplexIndex, Array<WeakReference<Parameter>>(params.getRawDataPointer(), params.size()));
	if(outParams.size() > 0) for (auto &o : items) o->setOutParams(outParams[multiplexIndex], multiplexIndex); //better than this ? should handle all ?

	prevMergedValue.ensureStorageAllocated(multiplexIndex+1);
	prevMergedValue.set(multiplexIndex, getMergedOutValue(multiplexIndex));

	omAsyncNotifier.addMessage(new OutputManagerEvent(OutputManagerEvent::OUTPUT_CHANGED));
}


void MappingOutputManager::updateOutputValues(int multiplexIndex, bool sendOnOutputChangedOnly)
{
	var value = getMergedOutValue(multiplexIndex);
	if (value.isVoid()) return; //possible if parameters have been deleted in another thread during process
	if (sendOnOutputChangedOnly && value == prevMergedValue[multiplexIndex]) return;

	{
		//a sequence's play thread walks the outputs : one removed on the message thread leaves the list only between two
		//frames, and is deleted after that
		const ScopedLock itemsLock(items.getLock());
		for (auto& i : items) i->setValue(value, multiplexIndex);
	}
	prevMergedValue.set(multiplexIndex, value);
}

void MappingOutputManager::updateOutputValue(MappingOutput * o, int multiplexIndex)
{
	if (forceDisabled) return;
	if (outParams.size() == 0) return;
	if (o == nullptr) return;

	const ScopedLock itemsLock(items.getLock());
	if (!items.contains(o)) return;
	o->setValue(getMergedOutValue(multiplexIndex), multiplexIndex);
}

var MappingOutputManager::getMergedOutValue(int multiplexIndex)
{
	var value;
	for (auto& o : outParams[multiplexIndex])
	{
		if (o.wasObjectDeleted()) return var();

		var val = o->getValue();
		if (!val.isArray()) value.append(val);
		else
		{
			for (int i = 0; i < val.size(); ++i)
			{
				value.append(val[i]);
			}
		}
	}

	return value;
}

void MappingOutputManager::addItemInternal(MappingOutput * o, var)
{
	o->addCommandHandlerListener(this);

	if (outParams.size() > 0)
	{
		for (int i = 0; i < outParams.size(); i++) o->setOutParams(outParams[i], i);
	}

	//for (int i = 0; i < getMultiplexCount(); i++) updateOutputValue(o, i);
}

void MappingOutputManager::removeItemInternal(MappingOutput * o)
{
	o->removeCommandHandlerListener(this);
}

void MappingOutputManager::commandChanged(BaseCommandHandler * h)
{
	MappingEditScope editScope(this); //updating the commands' value types can rebuild their parameters
	for (auto& o : items) o->updateCommandOutParams();
	if (!sendOnCommandChange) return;
	for (int i = 0; i < getMultiplexCount(); i++)
	{
		updateOutputValue(dynamic_cast<MappingOutput *>(h), i);
	}
}

void MappingOutputManager::commandUpdated(BaseCommandHandler * h)
{
	MappingEditScope editScope(this);
	for (auto& o : items) o->updateCommandOutParams();
	if (!sendOnCommandChange) return;
	for (int i = 0; i < getMultiplexCount(); i++) updateOutputValue(dynamic_cast<MappingOutput *>(h), i);
}

void MappingOutputManager::multiplexPreviewIndexChanged()
{
	omAsyncNotifier.addMessage(new OutputManagerEvent(OutputManagerEvent::OUTPUT_CHANGED));
}

InspectableEditor * MappingOutputManager::getEditorInternal(bool _isRoot, Array<Inspectable*> inspectables)
{
	return new MappingOutputManagerEditor(this, _isRoot);
}
