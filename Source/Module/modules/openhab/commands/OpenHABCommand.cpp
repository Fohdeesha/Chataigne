/*
  ==============================================================================

	OpenHABCommand.cpp
	Created: 23 Sep 2026

  ==============================================================================
*/

OpenHABCommand::OpenHABCommand(OpenHABModule* _module, CommandContext context, var params, Multiplex* multiplex) :
	BaseCommand(_module, context, params, multiplex),
	openHABModule(_module)
{
	item = addTargetParameter("Item", "The item to send the command to", &_module->valuesCC);
	item->excludeTypesFilter.add(Trigger::getTypeStringStatic());
	itemName = addStringParameter("Item Name", "Used when Item is not set : any openHAB item name, including items the module's filter leaves out", "");
	command = addStringParameter("Command", "The command as openHAB takes it, e.g. ON, OFF, 50, 120,100,50, UP, DOWN, STOP, PLAY, NEXT, INCREASE", "");
}

OpenHABCommand::~OpenHABCommand()
{
}

void OpenHABCommand::triggerInternal(int multiplexIndex)
{
	String name;
	if (Controllable* c = getLinkedTargetAs<Controllable>(item, multiplexIndex)) name = c->shortName;
	if (name.isEmpty()) name = getLinkedValue(itemName, multiplexIndex).toString().trim();

	openHABModule->queueRawCommand(name, getLinkedValue(command, multiplexIndex).toString());
}
