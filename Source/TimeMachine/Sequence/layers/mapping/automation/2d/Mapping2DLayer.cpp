/*
  ==============================================================================

    Mapping2DLayer.cpp
    Created: 23 Mar 2020 5:17:47pm
    Author:  bkupe

  ==============================================================================
*/

Mapping2DLayer::Mapping2DLayer(Sequence* s, var params) :
	AutomationMappingLayer(getTypeString(), s, params)
{
	helpID = "Mapping2DLayer";

	addChildControllableContainer(&curve);
	
	curve.setControlMode(Curve2D::AUTOMATION);
	curve.recorder = &recorder;
	recorder.input->typesFilter.add(Point2DParameter::getTypeStringStatic());

	setupAutomation((Automation*)curve.position->automation->automationContainer);
	((ParameterNumberAutomation*)curve.position->automation.get())->length->isSavable = false;
	((ParameterNumberAutomation*)curve.position->automation.get())->setLength(automation->length->floatValue());
	setupMappingInputParameter(curve.value);


	uiHeight->setValue(200);
}

Mapping2DLayer::~Mapping2DLayer()
{
}

void Mapping2DLayer::addDefaultContent()
{
	automation->addKey(0, 0, false);
	automation->addKey(sequence->totalTime->floatValue(), 1, false);
}


var Mapping2DLayer::getValueAtPosition(float position)
{
	Point<float> p = curve.getValueAtNormalizedPosition((float)automation->getNormalizedValueAtPosition(position));
	var result;
	result.append(p.x);
	result.append(p.y);
	return result;
}

void Mapping2DLayer::stopRecorderAndAddKeys()
{

	Array<AutomationRecorder::RecordValue> recordedValues = recorder.stopRecordingAndGetKeys();

	if (recordedValues.size() == 0) return;
	if (!recordedValues[0].value.isArray()) return;

	Array<Point<float>> points;
	Array<float> times;

	Array<Point<float>> positions;
	float totalLength = 0;
	Point<float> prevP(recordedValues[0].value[0], recordedValues[0].value[1]);

	for (auto& rv : recordedValues)
	{
		times.add(rv.time);
		Point<float> p(rv.value[0], rv.value[1]);
		totalLength += p.getDistanceFrom(prevP);
		
		positions.add(Point<float>(rv.time, totalLength));
		points.add(p);
		prevP.setXY(p.x, p.y);
	}

	if (totalLength == 0) return;
	
	for (auto& p : positions)
	{
		p.y /= totalLength;
	}
	curve.addFromPointsAndSimplify(points, true, times);
	automation->addFromPointsAndSimplifyBezier(positions);
}

void Mapping2DLayer::addKeyAtCurrentTimeFromInput()
{
	bool ok = false;
	var v = getRecorderInputValue(&ok);
	if (!ok) return;

	if (!v.isArray() || v.size() < 2)
	{
		NLOG(niceName, "Can't add key from input : recorder input is not a 2D value.");
		return;
	}

	Point<float> p((float)v[0], (float)v[1]);
	float t = sequence->currentTime->floatValue();

	//Append the captured point to the spatial curve. updateCurve() (called on add) recomputes curvePosition and length.
	//Note : a capture adds two objects (a spatial curve point + a timing key), so it takes two undo steps to fully revert.
	Curve2DKey* k = new Curve2DKey();
	k->setPosition(p);
	curve.addItem(k, var(), true);

	//Pin a timing key at the playhead pointing to the new point's normalized progression along the curve.
	float norm = curve.length->floatValue() > 0 ? k->curvePosition / curve.length->floatValue() : 0;

	if (AutomationKey* ak = getKeyToOverwriteAt(automation, t)) ak->value->setUndoableValue(ak->value->floatValue(), norm);
	else automation->addKey(t, norm, true);
}

SequenceLayerPanel* Mapping2DLayer::getPanel()
{
	return new Mapping2DLayerPanel(this);
}

SequenceLayerTimeline* Mapping2DLayer::getTimelineUI()
{
	return new Mapping2DTimeline(this);
}
