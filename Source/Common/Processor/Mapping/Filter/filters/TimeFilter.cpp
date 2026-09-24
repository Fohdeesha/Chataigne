/*
  ==============================================================================

	TimeFilter.cpp
	Created: 18 Jun 2021 8:44:36pm
	Author:  bkupe

  ==============================================================================
*/

TimeFilter::TimeFilter(StringRef name, var params, Multiplex* multiplex) :
	MappingFilter(name, params, multiplex, true)
{
	//The high-resolution counter : the millisecond one wraps every 49.7 days, and two updates within one millisecond read
	//an interval of 0. Filled after the resize : filled before, the start times stayed at 0 and the first interval was
	//the machine's uptime.
	deltaTimes.resize(getMultiplexCount());
	timesAtLastUpdate.resize(getMultiplexCount());
	timesAtLastUpdate.fill(Time::getMillisecondCounterHiRes() / 1000.0);
	deltaTimes.fill(0);
}

TimeFilter::~TimeFilter()
{
}

void TimeFilter::multiplexCountChanged()
{
	deltaTimes.resize(getMultiplexCount());
	timesAtLastUpdate.resize(getMultiplexCount());
	timesAtLastUpdate.fill(Time::getMillisecondCounterHiRes() / 1000.0);
	deltaTimes.fill(0);
}

MappingFilter::ProcessResult TimeFilter::processInternal(Array<Parameter*> sources, int multiplexIndex)
{
	double curTime = Time::getMillisecondCounterHiRes() / 1000.0;
	double lastUpdate = timesAtLastUpdate.size() > multiplexIndex ? timesAtLastUpdate.getUnchecked(multiplexIndex) : curTime;
	deltaTimes.set(multiplexIndex, jmax<double>(curTime - lastUpdate, 0));

	ProcessResult r = MappingFilter::processInternal(sources, multiplexIndex);

	timesAtLastUpdate.set(multiplexIndex, curTime);
	return r;
}

MappingFilter::ProcessResult TimeFilter::processSingleParameterInternal(Parameter* source, Parameter* out, int multiplexIndex)
{
	return processSingleParameterTimeInternal(source, out, multiplexIndex, deltaTimes[multiplexIndex]);
}
