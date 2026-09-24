/*
  ==============================================================================

    OSCOutputEditor.cpp
    Created: 3 Mar 2019 12:15:43pm
    Author:  bkupe

  ==============================================================================
*/

OSCOutputEditor::OSCOutputEditor(OSCOutput * output, bool isRoot) :
	BaseItemEditor(output, isRoot),
	zeroconfMenu("Auto detect")
{
	addAndMakeVisible(&zeroconfMenu);
	zeroconfMenu.addListener(this);
}

OSCOutputEditor::~OSCOutputEditor()
  {
  }

void OSCOutputEditor::resizedInternalHeaderItemInternal(Rectangle<int>& r)
{
	zeroconfMenu.setBounds(r.removeFromRight(60).reduced(0,2));
}

void OSCOutputEditor::showMenuAndSetupOutput()
{
	//the output, not this editor : the Inspector can rebuild (and delete this editor) while the menu is open
	WeakReference<Inspectable> weakOutput(item);
	ZeroconfManager::getInstance()->showMenuAndGetService("OSC", [weakOutput](ZeroconfManager::ServiceInfo* service)
		{
			OSCOutput* o = dynamic_cast<OSCOutput*>(weakOutput.get());
			if (service != nullptr && o != nullptr)
			{
				o->useLocal->setValue(service->isLocal);
				o->remoteHost->setValue(service->getIP());
				o->remotePort->setValue(service->port);
			}
		}
	);
}

void OSCOutputEditor::buttonClicked(Button * b)
{
	BaseItemEditor::buttonClicked(b);

	if (b == &zeroconfMenu) showMenuAndSetupOutput();
}
