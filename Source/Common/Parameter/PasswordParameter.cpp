/*
  ==============================================================================

	PasswordParameter.cpp
	Created: 23 Sep 2026

  ==============================================================================
*/

PasswordParameter::PasswordParameter(const String& niceName, const String& description, const String& initialValue, bool enabled) :
	StringParameter(niceName, description, initialValue, enabled)
{
	//set both, so hiding it is the default and nothing extra is written to the project
	hideInRemoteControl = true;
	defaultHideInRemoteControl = true;
}

ControllableUI* PasswordParameter::createDefaultUI(Array<Controllable*> controllables)
{
	Array<StringParameter*> parameters = getArrayAs<Controllable, StringParameter>(controllables);
	if (parameters.size() == 0) parameters = { this };
	return new PasswordParameterUI(parameters);
}


PasswordParameterUI::PasswordParameterUI(Array<StringParameter*> parameters) :
	StringParameterUI(parameters)
{
	//the base constructor put the plain value in the label
	valueLabel.setText(getMaskedText(), dontSendNotification);
}

String PasswordParameterUI::getMaskedText() const
{
	if (parameter == nullptr || parameter.wasObjectDeleted() || parameter->stringValue().isEmpty()) return {};
	return String::repeatedString(String::charToString((juce_wchar)0x2022), 8);
}

void PasswordParameterUI::editorShown(Label*, TextEditor& editor)
{
	//the label opened the editor with the dots : edit the real value, masked
	editor.setPasswordCharacter((juce_wchar)0x2022);
	editor.setText(stringParam->stringValue(), false);
	editor.selectAll();
}

void PasswordParameterUI::valueChanged(const var&)
{
	valueLabel.setText(getMaskedText(), dontSendNotification);
}

void PasswordParameterUI::labelTextChanged(Label* label)
{
	//the label now holds what was typed, in clear : store it, then mask the label again straight away. The
	//base class only sets the parameter, and an unchanged value would never come back through valueChanged.
	StringParameterUI::labelTextChanged(label);
	valueLabel.setText(getMaskedText(), dontSendNotification);
}
