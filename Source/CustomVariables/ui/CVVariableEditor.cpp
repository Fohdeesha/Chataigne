/*
  ==============================================================================

	CVVariableEditor.cpp
	Created: 19 Sep 2026

  ==============================================================================
*/

CVVariableEditor::CVVariableEditor(CVVariable* v, bool isRoot) :
	GenericControllableItemEditor(v, isRoot),
	variable(v)
{
	resetAndBuild();
}

CVVariableEditor::~CVVariableEditor()
{
}

void CVVariableEditor::resetAndBuild()
{
	//deliberately skipping GenericControllableItemEditor::resetAndBuild(), which is a no-op
	GenericControllableContainerEditor::resetAndBuild();
}

bool CVVariableEditor::shouldShowControllable(Controllable* c)
{
	//the variable's own controllable already lives in the header
	if (variable != nullptr && c == variable->controllable) return false;
	return GenericControllableItemEditor::shouldShowControllable(c);
}
