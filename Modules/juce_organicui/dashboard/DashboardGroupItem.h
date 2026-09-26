
#pragma once

class DashboardGroupItem :
	public DashboardItem,
	public DashboardItemManager::ManagerListener,
	public DashboardFeedbackBroadcaster::FeedbackListener
{
public:
	DashboardGroupItem();
	virtual ~DashboardGroupItem();

	//The dashboard listens to its own items only : the items in a group never updated in the web clients. The group
	//passes their feedback on (and a group in a group does the same).
	void itemAdded(DashboardItem* item) override;
	void itemsAdded(juce::Array<DashboardItem*> items) override;
	void itemRemoved(DashboardItem* item) override;
	void itemsRemoved(juce::Array<DashboardItem*> items) override;
	void parameterFeedback(juce::var data) override { notifyDataFeedback(data); }
	void dashboardFeedback(juce::var data) override { notifyDashboardFeedback(data); }


	FloatParameter* borderWidth;
	ColorParameter* borderColor;
	ColorParameter* backgroundColor;

	DashboardItemManager itemManager;

	juce::var getServerData() override;

	juce::var getJSONData(bool includeNonOverriden = false) override;
	void loadJSONDataItemInternal(juce::var data) override;

	bool paste() override;
	
	virtual DashboardItemUI* createUI() override;

	juce::String getTypeString() const override { return "DashboardGroupItem"; }
	static DashboardGroupItem* create(juce::var) { return new DashboardGroupItem(); }
};