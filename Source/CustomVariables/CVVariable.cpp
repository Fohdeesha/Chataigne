/*
  ==============================================================================

	CVVariable.cpp
	Created: 19 Sep 2026

  ==============================================================================
*/

CVVariable::CVVariable(var params) :
	CVVariable(nullptr, params)
{
}

CVVariable::CVVariable(Controllable* c, var params) :
	GenericControllableItem(c, params)
{
	setupBinding();
}

CVVariable::~CVVariable()
{
	//the binding holds listeners on foreign parameters, tear it down before our members go
	binding.reset();
}

void CVVariable::setupBinding()
{
	if (controllable == nullptr) return; //base ctor already logged the error
	if (binding != nullptr) return;

	binding.reset(new CVBinding(this));
	addChildControllableContainer(binding.get());

	//GenericControllableItem sets editorCanBeCollapsed = false, because upstream's item editor has
	//no content to hide : its resetAndBuild() is a no-op. Ours builds the Binding section, and with
	//that flag false GenericControllableContainerEditor draws no expand arrow at all, so a variable
	//row stayed one line and the binding could not be reached from the UI - the item is created
	//collapsed. Let the row expand, and leave it collapsed so a group's list stays compact.
	editorCanBeCollapsed = true;
}

void CVVariable::onContainerParameterChangedInternal(Parameter* p)
{
	GenericControllableItem::onContainerParameterChangedInternal(p);

	if (binding != nullptr && p == controllable) binding->controlValueChanged();
}

void CVVariable::onContainerTriggerTriggered(Trigger* t)
{
	GenericControllableItem::onContainerTriggerTriggered(t);

	if (binding != nullptr && t == controllable) binding->controlTriggered();
}

var CVVariable::getJSONData(bool includeNonOverriden)
{
	var data = GenericControllableItem::getJSONData(includeNonOverriden);

	if (binding != nullptr)
	{
		var bData = binding->getJSONData(includeNonOverriden);
		if (!bData.isVoid() && bData.getDynamicObject() != nullptr && bData.getDynamicObject()->getProperties().size() > 0)
			data.getDynamicObject()->setProperty("binding", bData);
	}

	return data;
}

void CVVariable::loadJSONDataInternal(var data)
{
	GenericControllableItem::loadJSONDataInternal(data);

	if (binding != nullptr && data.getDynamicObject() != nullptr && data.getDynamicObject()->hasProperty("binding"))
	{
		binding->loadJSONData(data.getProperty("binding", var()));
	}
}

InspectableEditor* CVVariable::getEditorInternal(bool isRoot, Array<Inspectable*> inspectables)
{
	return new CVVariableEditor(this, isRoot);
}
