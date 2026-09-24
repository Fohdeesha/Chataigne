/*
  ==============================================================================

	OSCQueryModuleEditor.cpp
	Created: 3 Mar 2019 12:36:47pm
	Author:  bkupe

  ==============================================================================
*/

#include "Module/ModuleIncludes.h"

OSCQueryModuleOutputEditor::OSCQueryModuleOutputEditor(OSCQueryOutput* output, bool isRoot) :
	EnablingControllableContainerEditor(output, isRoot),
	zeroconfMenu("Auto detect")
{
	addAndMakeVisible(&zeroconfMenu);
	zeroconfMenu.addListener(this);
}

OSCQueryModuleOutputEditor::~OSCQueryModuleOutputEditor()
{
}

void OSCQueryModuleOutputEditor::resizedInternalHeader(Rectangle<int>& r)
{
	EnablingControllableContainerEditor::resizedInternalHeader(r);
	zeroconfMenu.setBounds(r.removeFromRight(60).reduced(2));
}

void OSCQueryModuleOutputEditor::showMenuAndSetupOutput()
{
	//the output container, not this editor : the Inspector can rebuild (and delete this editor) while the menu is open
	WeakReference<Inspectable> weakOutput(container.get());
	ZeroconfManager::getInstance()->showMenuAndGetService("OSCQuery", [weakOutput](ZeroconfManager::ServiceInfo* service)
		{
			OSCQueryOutput* output = dynamic_cast<OSCQueryOutput*>(weakOutput.get());
			if (service != nullptr && output != nullptr)
			{
				GenericOSCQueryModule* o = output->module;
				bool isSame = o->useLocal->boolValue() == service->isLocal
					&& o->remoteHost->stringValue() == service->getIP()
					&& o->remotePort->intValue() == service->port;

				if (isSame)
				{
					o->syncTrigger->trigger();
					return;
				}

				o->useLocal->setValue(service->isLocal);
				o->remoteHost->setValue(service->getIP());
				o->remotePort->setValue(service->port);

			}
		}
	);
}

void OSCQueryModuleOutputEditor::buttonClicked(Button* b)
{
	EnablingControllableContainerEditor::buttonClicked(b);
	if (b == &zeroconfMenu) showMenuAndSetupOutput();
}
