/*
  ==============================================================================

	CVValueMap.h
	Created: 19 Sep 2026

	An invertible transform between a Custom Variable's value (the "control" side)
	and the value sent to / received from a device (the "device" side).

	A two-way binding can only exist if the transform can be run backwards, which
	is why this is a declared mapping rather than a filter chain: MappingFilter
	subclasses (Remap, Convert To Integer, String Replace...) have no inverse.

  ==============================================================================
*/

#pragma once

class CVValueMapEntry :
	public BaseItem
{
public:
	CVValueMapEntry();
	~CVValueMapEntry();

	StringParameter* controlValue;
	StringParameter* deviceValue;

	static CVValueMapEntry* create(var) { return new CVValueMapEntry(); }

	DECLARE_TYPE("Entry")
};


class CVValueMap :
	public ControllableContainer
{
public:
	CVValueMap(const String& name = "Value Map");
	~CVValueMap();

	enum MapMode { NONE, TABLE, LINEAR };
	enum OutType { SAME_AS_CONTROL, T_FLOAT, T_INT, T_BOOL, T_STRING };

	EnumParameter* mode;
	EnumParameter* outputType;

	//TABLE
	BaseManager<CVValueMapEntry> table;

	//LINEAR
	FloatParameter* inMin;
	FloatParameter* inMax;
	FloatParameter* outMin;
	FloatParameter* outMax;
	BoolParameter* clampLinear;

	MapMode getMode() const;
	OutType getOutType() const;

	//control -> device. controlType is needed because "Same as control" output means exactly that,
	//and the parameter built by getOutputControllableType() must agree with what forward() returns.
	var forward(const var& controlValue, Controllable::Type controlType, bool* success = nullptr);
	//device -> control
	var inverse(const var& deviceValue, Controllable::Type controlType, bool* success = nullptr);

	//the Controllable type string that the outgoing value should be carried by,
	//used to build the parameter that feeds the Send To commands
	String getOutputControllableType(Controllable::Type controlType) const;

	//normalises a var to a comparable key so "true"/"1"/"ON" all match a bool
	static String keyFor(const var& v);
	static bool keysMatch(const String& a, const String& b);

	//Both report failure rather than guessing. A device value that cannot honestly be read as the
	//target type is ignored, exactly like a value missing from a Table : guessing would silently
	//turn "joint" into true and move a control to the wrong state.
	static var coerce(const String& s, Controllable::Type t, bool* ok = nullptr);
	//coerce a var without going through a string, so float precision survives the round trip
	static var coerceVar(const var& v, Controllable::Type t, bool* ok = nullptr);

	void onContainerParameterChanged(Parameter* p) override;

	var getJSONData(bool includeNonOverriden = false) override;
	void loadJSONDataInternal(var data) override;

	InspectableEditor* getEditorInternal(bool isRoot, Array<Inspectable*> inspectables = Array<Inspectable*>()) override;
};
