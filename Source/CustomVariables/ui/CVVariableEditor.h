/*
  ==============================================================================

	CVVariableEditor.h
	Created: 19 Sep 2026

	GenericControllableItemEditor neuters resetAndBuild() so that a Custom Variable
	row shows only its controllable in the header. That also means child containers
	never render, so the binding would be invisible. This restores the normal build
	while keeping the controllable itself in the header where it belongs.

  ==============================================================================
*/

#pragma once

class CVVariableEditor :
	public GenericControllableItemEditor
{
public:
	CVVariableEditor(CVVariable* v, bool isRoot);
	~CVVariableEditor();

	CVVariable* variable;

	void resetAndBuild() override;
	bool shouldShowControllable(Controllable* c) override;
};
