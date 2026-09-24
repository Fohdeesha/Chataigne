/*
  ==============================================================================

    WakeOnLanCommandEditor.cpp
    Created: 2 Apr 2019 5:56:12pm
    Author:  bkupe

  ==============================================================================
*/

WakeOnLanCommandEditor::WakeOnLanCommandEditor(WakeOnLanCommand * wolCommand, bool isRoot) :
	GenericControllableContainerEditor(wolCommand, isRoot),
	zeroconfMenu("Auto detect"),
    wolCommand(wolCommand)

{
	zeroconfMenu.addListener(this);
	addAndMakeVisible(&zeroconfMenu);
}

WakeOnLanCommandEditor::~WakeOnLanCommandEditor()
{
}

void WakeOnLanCommandEditor::resizedInternalHeader(Rectangle<int>& r)
{
	zeroconfMenu.setBounds(r.removeFromRight(60).reduced(2));
	GenericControllableContainerEditor::resizedInternalHeader(r);
}

void WakeOnLanCommandEditor::showMenuAndSetupMacAdddress()
{
	//the command, not this editor : the Inspector can rebuild (and delete this editor) while the menu is open
	WeakReference<Inspectable> weakCommand(wolCommand);
	ZeroconfManager::getInstance()->showMenuAndGetService("Workstation", [weakCommand](ZeroconfManager::ServiceInfo* service)
		{
			WakeOnLanCommand* cmd = dynamic_cast<WakeOnLanCommand*>(weakCommand.get());
			if (service != nullptr && cmd != nullptr)
			{
				String s = service->name.substring(service->name.indexOf("[") + 1, service->name.indexOf("]"));
				cmd->macAddress->setValue(s);
			}
		}
	);
}

void WakeOnLanCommandEditor::buttonClicked(Button * b)
{
	GenericControllableContainerEditor::buttonClicked(b);
	if (b == &zeroconfMenu) showMenuAndSetupMacAdddress();
}
