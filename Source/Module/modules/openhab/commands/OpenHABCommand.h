/*
  ==============================================================================

	OpenHABCommand.h
	Created: 23 Sep 2026

  ==============================================================================
*/

#pragma once

//Sends a raw item command, for what a value cannot express : UP, STOP, NEXT, INCREASE, or the same command twice.
class OpenHABCommand :
	public BaseCommand
{
public:
	OpenHABCommand(OpenHABModule* _module, CommandContext context, var params, Multiplex* multiplex = nullptr);
	~OpenHABCommand();

	OpenHABModule* openHABModule;

	TargetParameter* item;
	StringParameter* itemName;
	StringParameter* command;

	void triggerInternal(int multiplexIndex) override;

	static BaseCommand* create(ControllableContainer* module, CommandContext context, var params, Multiplex* multiplex) { return new OpenHABCommand((OpenHABModule*)module, context, params, multiplex); }
};
