/*
  ==============================================================================

	CVVariable.h
	Created: 19 Sep 2026

	A Custom Variable. Identical to the stock GenericControllableItem it replaces,
	plus an optional two-way CVBinding.

	The JSON "type" string is inherited unchanged (GenericControllableItem stores
	typeAtCreation), so projects saved before this existed load as CVVariables with
	an inert binding, and nothing about the variable's control address moves.

  ==============================================================================
*/

#pragma once

class CVBinding;

class CVVariable :
	public GenericControllableItem
{
public:
	CVVariable(var params = var());
	CVVariable(Controllable* c, var params = var());
	~CVVariable();

	std::unique_ptr<CVBinding> binding;

	void setupBinding();

	void onContainerParameterChangedInternal(Parameter* p) override;
	void onContainerTriggerTriggered(Trigger* t) override;

	static CVVariable* create(var params) { return new CVVariable(params); }

	var getJSONData(bool includeNonOverriden = false) override;
	void loadJSONDataInternal(var data) override;

	InspectableEditor* getEditorInternal(bool isRoot, Array<Inspectable*> inspectables = Array<Inspectable*>()) override;
};
