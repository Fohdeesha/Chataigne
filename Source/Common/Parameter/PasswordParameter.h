/*
  ==============================================================================

	PasswordParameter.h
	Created: 23 Sep 2026

	A string parameter for secrets (passwords, API tokens). The interface shows dots instead of the value, the
	editor masks what is typed, and the parameter is left out of OSCQuery listings. The value is still saved in
	the project file in plain text, like any other string.

  ==============================================================================
*/

#pragma once

class PasswordParameter :
	public StringParameter
{
public:
	PasswordParameter(const String& niceName, const String& description, const String& initialValue = "", bool enabled = true);
	~PasswordParameter() override {}

	ControllableUI* createDefaultUI(Array<Controllable*> controllables = {}) override;

	//a direct OSCQuery GET of the address answered the plain value (the listing already left it out). It stays in the
	//project file, and so in the remote control's /sessionFile, like any string.
	var getRemoteControlValue() override { return stringValue().isEmpty() ? String() : String("********"); }
};

class PasswordParameterUI :
	public StringParameterUI
{
public:
	PasswordParameterUI(Array<StringParameter*> parameters);
	~PasswordParameterUI() override {}

	//a fixed number of dots, so the display does not give the length away either
	String getMaskedText() const;

	void editorShown(Label* label, TextEditor& editor) override;

protected:
	void valueChanged(const var& v) override;
	void labelTextChanged(Label* label) override;

private:
	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PasswordParameterUI)
};
