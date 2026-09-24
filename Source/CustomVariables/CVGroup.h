/*
  ==============================================================================

    CVGroup.h
    Created: 15 Feb 2018 3:49:35pm
    Author:  Ben

  ==============================================================================
*/

#pragma once

class CVPreset;
class CVPresetManager;

class CVGroup :
	public BaseItem,
	public Morpher::MorpherListener,
	public Thread,
	public GenericControllableManager::ManagerListener
{
public:
	CVGroup(const String &name = "Group");
	~CVGroup();

	ControllableContainer params;

	enum ControlMode { FREE, WEIGHTS, VORONOI, GRADIENT_BAND };
	EnumParameter * controlMode;
	Trigger* randomize;

	class ValuesManager :
		public GenericControllableManager
	{
	public:
		ValuesManager();
		~ValuesManager();

		//shadows the non-virtual base version so CVVariables (bindable) are created instead
		GenericControllableItem* addItemFrom(Controllable* c, bool copyValue = true);

		void addAllItemsToDashboard(Dashboard* d);

		InspectableEditor* getEditorInternal(bool isRoot, Array<Inspectable*> inspectables = Array<Inspectable*>()) override;
	};
	ValuesManager values;

	std::unique_ptr<CVPresetManager> pm;
	std::unique_ptr<Morpher> morpher;

	//Animated interpolation
	Automation defaultInterpolation;

	CVPreset* targetPreset;
	Automation* interpolationAutomation;
	WeakReference<Inspectable> automationRef;
	float interpolationTime;
	FloatParameter* interpolationProgress;

	CriticalSection interpolationLock;

	//Built on the message thread before the interpolation thread starts (a preset copy links itself to every variable),
	//used by that thread, replaced only once it has stopped
	std::unique_ptr<CVPreset> interpolationTarget;
	std::unique_ptr<Automation> interpolationCurve;
	Array<var> interpolationSource;

	void addItemFromParameter(Parameter* source, bool linkAsMaster = true);
	void addItemsFromGroup(CVGroup* source);

	void itemAdded(GenericControllableItem* item) override;
	void itemsAdded(Array<GenericControllableItem*> item) override;
	
	void setValuesToPreset(CVPreset * preset);
	void lerpPresets(CVPreset * p1, CVPreset * p2, float weight);
	void lerpPresets(Array<var> sourceValues, CVPreset* endPreset, float weight);

	void goToPreset(CVPreset* p, float time, Automation* curve);
	void stopInterpolation();

	void randomizeValues();

	void computeValues();
	Array<float> getNormalizedPresetWeights();

	void weightsUpdated() override;

	void onControllableFeedbackUpdateInternal(ControllableContainer * cc, Controllable * c) override;

	var getJSONData(bool includeNonOverriden = false) override;
	void loadJSONDataInternal(var data) override;

	void run() override;


	DECLARE_TYPE("CVGroup")
};