/*
  ==============================================================================

	CVValueMap.cpp
	Created: 19 Sep 2026

  ==============================================================================
*/

CVValueMapEntry::CVValueMapEntry() :
	BaseItem("Entry", false)
{
	itemDataType = "Entry";

	controlValue = addStringParameter("Control", "Value on the Custom Variable side", "");
	deviceValue = addStringParameter("Device", "Value on the device side", "");
}

CVValueMapEntry::~CVValueMapEntry()
{
}


// ---------------------------------------------------------------------------

CVValueMap::CVValueMap(const String& name) :
	ControllableContainer(name),
	table("Table")
{
	editorCanBeCollapsed = true;
	editorIsCollapsed = true;

	mode = addEnumParameter("Mode", "How the control value is transformed before being sent, and back again on feedback.\n\
None : send and receive the value as-is.\n\
Table : explicit pairs, invertible by lookup. Use for bool, enum, int and string values.\n\
Linear : range remap, inverted analytically. Use for continuous values.");
	mode->addOption("None", NONE)->addOption("Table", TABLE)->addOption("Linear", LINEAR);

	outputType = addEnumParameter("Output Type", "The type of the value handed to the Send To commands. 'Same as control' keeps the variable's own type.");
	outputType->addOption("Same as control", SAME_AS_CONTROL)->addOption("Float", T_FLOAT)->addOption("Integer", T_INT)->addOption("Boolean", T_BOOL)->addOption("String", T_STRING);

	table.selectItemWhenCreated = false;
	table.editorCanBeCollapsed = true;
	addChildControllableContainer(&table);

	inMin = addFloatParameter("In Min", "Low end of the control-side range", 0);
	inMax = addFloatParameter("In Max", "High end of the control-side range", 1);
	outMin = addFloatParameter("Out Min", "Low end of the device-side range", 0);
	outMax = addFloatParameter("Out Max", "High end of the device-side range", 100);
	clampLinear = addBoolParameter("Clamp", "Clamp the result to the target range", true);

	onContainerParameterChanged(mode);
}

CVValueMap::~CVValueMap()
{
}

CVValueMap::MapMode CVValueMap::getMode() const
{
	return mode->getValueDataAsEnum<MapMode>();
}

CVValueMap::OutType CVValueMap::getOutType() const
{
	return outputType->getValueDataAsEnum<OutType>();
}

void CVValueMap::onContainerParameterChanged(Parameter* p)
{
	ControllableContainer::onContainerParameterChanged(p);

	if (p == mode)
	{
		MapMode m = getMode();
		table.hideInEditor = m != TABLE;
		inMin->hideInEditor = inMax->hideInEditor = outMin->hideInEditor = outMax->hideInEditor = clampLinear->hideInEditor = m != LINEAR;
		queuedNotifier.addMessage(new ContainerAsyncEvent(ContainerAsyncEvent::ControllableContainerNeedsRebuild, this));
	}
}


// --- key normalisation -------------------------------------------------------

String CVValueMap::keyFor(const var& v)
{
	if (v.isBool()) return ((bool)v) ? "true" : "false";
	if (v.isInt() || v.isInt64()) return String((int64)v);
	if (v.isDouble())
	{
		double d = (double)v;
		if (d == (double)(int64)d) return String((int64)d);
		return String(d, 6);
	}
	if (v.isArray())
	{
		StringArray sa;
		for (int i = 0; i < v.size(); ++i) sa.add(keyFor(v[i]));
		return sa.joinIntoString(",");
	}
	return v.toString();
}

static bool cvIsNumeric(const String& s, double& out)
{
	String t = s.trim();
	if (t.isEmpty()) return false;
	if (!t.containsOnly("0123456789+-.eE")) return false;
	//reject strings that merely start with a digit but are not numbers
	if (!CharacterFunctions::isDigit(t[t.startsWithChar('-') || t.startsWithChar('+') ? 1 : 0])
		&& !t.substring(t.startsWithChar('-') || t.startsWithChar('+') ? 1 : 0).startsWithChar('.')) return false;
	out = t.getDoubleValue();
	return true;
}

static bool cvBoolish(const String& s, bool& out)
{
	String t = s.trim().toLowerCase();
	if (t == "true" || t == "on" || t == "yes" || t == "1") { out = true; return true; }
	if (t == "false" || t == "off" || t == "no" || t == "0") { out = false; return true; }
	return false;
}

bool CVValueMap::keysMatch(const String& a, const String& b)
{
	if (a.trim().equalsIgnoreCase(b.trim())) return true;

	double da, db;
	if (cvIsNumeric(a, da) && cvIsNumeric(b, db)) return std::abs(da - db) < 1e-9;

	bool ba, bb;
	if (cvBoolish(a, ba) && cvBoolish(b, bb)) return ba == bb;

	return false;
}

var CVValueMap::coerce(const String& s, Controllable::Type t, bool* ok)
{
	if (ok != nullptr) *ok = true;

	bool b = false;
	double d = 0;

	switch (t)
	{
	case Controllable::BOOL:
		if (cvBoolish(s, b)) return b;
		if (cvIsNumeric(s, d)) return d != 0;
		break;	//"joint" is not a boolean : say so instead of returning isNotEmpty()

	case Controllable::INT:
		if (cvBoolish(s, b)) return b ? 1 : 0;
		if (cvIsNumeric(s, d)) return (int)d;
		break;

	case Controllable::FLOAT:
		if (cvBoolish(s, b)) return b ? 1.f : 0.f;
		if (cvIsNumeric(s, d)) return (float)d;
		break;

	default:
		return s;
	}

	if (ok != nullptr) *ok = false;
	return var();
}

var CVValueMap::coerceVar(const var& v, Controllable::Type t, bool* ok)
{
	if (ok != nullptr) *ok = true;

	switch (t)
	{
	case Controllable::BOOL:
		if (v.isBool()) return v;
		if (v.isString()) return coerce(v.toString(), t, ok);
		if (v.isArray()) break;
		return (double)v != 0;

	case Controllable::INT:
		if (v.isString()) return coerce(v.toString(), t, ok);
		if (v.isArray()) break;
		return (int)v;

	case Controllable::FLOAT:
		if (v.isString()) return coerce(v.toString(), t, ok);
		if (v.isArray()) break;
		return (float)v;	//no string round trip : 33.333332f stays 33.333332f

	case Controllable::STRING:
		return keyFor(v);

	default:
		return v;
	}

	if (ok != nullptr) *ok = false;
	return var();
}

Controllable::Type cvOutTypeToControllableType(CVValueMap::OutType t, Controllable::Type controlType)
{
	switch (t)
	{
	case CVValueMap::T_FLOAT: return Controllable::FLOAT;
	case CVValueMap::T_INT: return Controllable::INT;
	case CVValueMap::T_BOOL: return Controllable::BOOL;
	case CVValueMap::T_STRING: return Controllable::STRING;
	case CVValueMap::SAME_AS_CONTROL:
	default: return controlType;
	}
}

String CVValueMap::getOutputControllableType(Controllable::Type controlType) const
{
	Controllable::Type t = cvOutTypeToControllableType(getOutType(), controlType);

	//an enum or target on the control side has no meaningful wire form, carry it as a string
	switch (t)
	{
	case Controllable::FLOAT: return FloatParameter::getTypeStringStatic();
	case Controllable::INT: return IntParameter::getTypeStringStatic();
	case Controllable::BOOL: return BoolParameter::getTypeStringStatic();
	case Controllable::POINT2D: return Point2DParameter::getTypeStringStatic();
	case Controllable::POINT3D: return Point3DParameter::getTypeStringStatic();
	case Controllable::COLOR: return ColorParameter::getTypeStringStatic();
	default: return StringParameter::getTypeStringStatic();
	}
}


// --- the transform -----------------------------------------------------------

var CVValueMap::forward(const var& controlValue, Controllable::Type controlType, bool* success)
{
	if (success != nullptr) *success = true;

	//whatever this returns must be carried by the parameter getOutputControllableType() builds
	const Controllable::Type outT = cvOutTypeToControllableType(getOutType(), controlType);

	switch (getMode())
	{
	case TABLE:
	{
		const String k = keyFor(controlValue);

		//two passes so an explicit "0" entry is never stolen by a "false" entry
		for (auto& e : table.items) if (e->controlValue->stringValue().trim().equalsIgnoreCase(k.trim())) return coerce(e->deviceValue->stringValue(), outT, success);
		for (auto& e : table.items) if (keysMatch(e->controlValue->stringValue(), k)) return coerce(e->deviceValue->stringValue(), outT, success);

		if (success != nullptr) *success = false;
		return var();
	}

	case LINEAR:
	{
		const float a = inMin->floatValue(), b = inMax->floatValue();
		const float c = outMin->floatValue(), d = outMax->floatValue();
		if (std::abs(b - a) < 1e-12f)
		{
			if (success != nullptr) *success = false;
			return var();
		}
		float r = c + ((float)controlValue - a) * (d - c) / (b - a);
		if (clampLinear->boolValue()) r = jlimit(jmin(c, d), jmax(c, d), r);
		return coerceVar(r, outT, success);
	}

	case NONE:
	default:
		return coerceVar(controlValue, outT, success);
	}
}

var CVValueMap::inverse(const var& deviceValue, Controllable::Type controlType, bool* success)
{
	if (success != nullptr) *success = true;

	switch (getMode())
	{
	case TABLE:
	{
		const String k = keyFor(deviceValue);

		for (auto& e : table.items) if (e->deviceValue->stringValue().trim().equalsIgnoreCase(k.trim())) return coerce(e->controlValue->stringValue(), controlType, success);
		for (auto& e : table.items) if (keysMatch(e->deviceValue->stringValue(), k)) return coerce(e->controlValue->stringValue(), controlType, success);

		if (success != nullptr) *success = false;
		return var();
	}

	case LINEAR:
	{
		const float a = inMin->floatValue(), b = inMax->floatValue();
		const float c = outMin->floatValue(), d = outMax->floatValue();
		if (std::abs(d - c) < 1e-12f)
		{
			if (success != nullptr) *success = false;
			return var();
		}
		float r = a + ((float)deviceValue - c) * (b - a) / (d - c);
		if (clampLinear->boolValue()) r = jlimit(jmin(a, b), jmax(a, b), r);
		return coerceVar(r, controlType, success);
	}

	case NONE:
	default:
		return coerceVar(deviceValue, controlType, success);
	}
}


var CVValueMap::getJSONData(bool includeNonOverriden)
{
	var data = ControllableContainer::getJSONData(includeNonOverriden);
	var tData = table.getJSONData(includeNonOverriden);
	if (!tData.isVoid() && tData.getDynamicObject() != nullptr && tData.getDynamicObject()->getProperties().size() > 0)
		data.getDynamicObject()->setProperty("table", tData);
	return data;
}

void CVValueMap::loadJSONDataInternal(var data)
{
	ControllableContainer::loadJSONDataInternal(data);
	if (data.getDynamicObject() != nullptr && data.getDynamicObject()->hasProperty("table")) table.loadJSONData(data.getProperty("table", var()));
	onContainerParameterChanged(mode);
}

InspectableEditor* CVValueMap::getEditorInternal(bool isRoot, Array<Inspectable*> inspectables)
{
	//see the note in CVBinding::getEditorInternal : buildAtCreation false leaves an uncollapsed
	//section drawn as open but empty until it is collapsed and expanded again
	return new GenericControllableContainerEditor(this, isRoot);
}
