/*
  ==============================================================================

    TimeTriggerManagerUI.cpp
    Created: 10 Dec 2016 12:23:33pm
    Author:  Ben

  ==============================================================================
*/

TimeTriggerManagerUI::TimeTriggerManagerUI(TriggerLayerTimeline * _timeline, TimeTriggerManager * manager) :
	BaseManagerUI("Triggers", manager, false),
	timeline(_timeline),
	miniMode(false)
{
	addItemText = "Add Trigger";
	animateItemOnAdd = false;
	transparentBG = true;

	addItemBT->setVisible(false);

	noItemText = "To add triggers, double click here";

	manager->selectionManager->addSelectionListener(this);

	resizeOnChildBoundsChanged = false;
	addExistingItems();
}

TimeTriggerManagerUI::~TimeTriggerManagerUI()
{
	manager->selectionManager->removeSelectionListener(this);
	if (InspectableSelector::getInstanceWithoutCreating()) InspectableSelector::getInstance()->removeSelectorListener(this);
}

void TimeTriggerManagerUI::setMiniMode(bool value)
{
	if (miniMode == value) return;
	miniMode = value;

	for (auto& i : itemsUI) i->setInterceptsMouseClicks(!miniMode, !miniMode);
}

void TimeTriggerManagerUI::resized()
{
	updateContent();
}

void TimeTriggerManagerUI::updateContent()
{
	for (auto &ttui : itemsUI)
	{
		placeTimeTriggerUI(ttui);
		if(ttui->item->isSelected) ttui->toFront(true);
		else ttui->toBack();
	}
}

void TimeTriggerManagerUI::placeTimeTriggerUI(TimeTriggerUI * ttui)
{
	int tx = timeline->getXForTime(ttui->item->time->floatValue());
	float totalTime = timeline->item->sequence->totalTime->floatValue();
	int maxX = timeline->getXForTime(totalTime);
	int triggerWidth = 0;
	float length = ttui->item->length->floatValue();
	if (length > 0.f)
		triggerWidth = jmax(0, timeline->getXForTime(ttui->item->time->floatValue() + length) - tx);

	if (tx + ttui->labelWidth > maxX && tx <= maxX)
	{
		int diff = ttui->labelWidth - 1;
		tx -= diff;
		ttui->startXOffset = diff;
		ttui->flagXOffset = 0;
	}
	else
	{
		ttui->startXOffset = 0;
		ttui->flagXOffset = 0;
	}
	ttui->triggerWidth = triggerWidth;
	ttui->setBounds(tx, 0, ttui->labelWidth + triggerWidth + 1, getHeight());
	ttui->resized();
	ttui->repaint();
}

void TimeTriggerManagerUI::mouseDown(const MouseEvent & e)
{
	BaseManagerUI::mouseDown(e);

	if (e.eventComponent == this)
	{
		if (e.mods.isLeftButtonDown())
		{
			if (e.mods.isAltDown())
			{
				if (manager->managerFactory == nullptr || manager->managerFactory->defs.size() == 1)
				{
					float time = timeline->getTimeForX(getMouseXYRelative().x);
					manager->addTriggerAt(time, getMouseXYRelative().y * 1.f / getHeight());
				}
			}
		}
	}
}

void TimeTriggerManagerUI::mouseDoubleClick(const MouseEvent & e)
{
	if (miniMode) return;
	if (manager->managerFactory != nullptr && manager->managerFactory->defs.size() > 1) return;

	float time = timeline->getTimeForX(getMouseXYRelative().x);
	manager->addTriggerAt(time, getMouseXYRelative().y*1.f / getHeight());
}

void TimeTriggerManagerUI::addItemFromMenu(bool isFromAddButton, Point<int> mouseDownPos)
{
	if (isFromAddButton || miniMode) return;

	float time = timeline->getTimeForX(mouseDownPos.x);
	manager->addTriggerAt(time, mouseDownPos.y*1.f / getHeight());
}

void TimeTriggerManagerUI::addItemFromMenu(TimeTrigger* t, bool, Point<int> mouseDownPos)
{
	float time = timeline->getTimeForX(mouseDownPos.x);
	t->time->setValue(time);
	t->flagY->setValue(mouseDownPos.y * 1.f / getHeight());
	manager->addItem(t);
}

void TimeTriggerManagerUI::addItemUIInternal(TimeTriggerUI * ttui)
{
	ttui->addTriggerUIListener(this);
}

void TimeTriggerManagerUI::removeItemUIInternal(TimeTriggerUI * ttui)
{
	ttui->removeTriggerUIListener(this);
}

void TimeTriggerManagerUI::timeTriggerMouseDown(TimeTriggerUI* ttui, const MouseEvent& e)
{
	snapTimes.clear();
	if (getSnapTimesFunc != nullptr) getSnapTimesFunc(&snapTimes);
}

void TimeTriggerManagerUI::scaleSelectedLengths(TimeTrigger* draggedItem, float diffTime)
{
	//Stretching a selection along time should stretch its bars by the same factor, otherwise the bars
	//keep their absolute length and no longer line up with the events they were placed against.
	//Mirrors BaseItem::scalePosition : same anchor (the far end of the selection) and same factor.
	Array<BaseItem*> items = InspectableSelectionManager::activeSelectionManager->getInspectablesAs<BaseItem>();
	if (items.size() < 2) return;

	float minTime = items[0]->movePositionReference.x;
	float maxTime = items[0]->movePositionReference.x;
	BaseItem* firstItem = items[0];
	for (auto& i : items)
	{
		if (i->isUILocked->boolValue()) continue;
		float t = i->movePositionReference.x;
		if (t < minTime) { minTime = t; firstItem = i; }
		else if (t > maxTime) maxTime = t;
	}

	float anchor = (draggedItem == firstItem) ? maxTime : minTime;
	float anchorDiff = draggedItem->movePositionReference.x - anchor;
	if (anchorDiff == 0) return;

	float factor = 1 + diffTime / anchorDiff;
	if (factor < 0) factor = 0;

	for (auto& i : items)
	{
		if (i->isUILocked->boolValue()) continue;
		if (auto* tt = dynamic_cast<TimeTrigger*>(i))
			if (tt->lengthMoveReference > 0) tt->length->setValue(tt->lengthMoveReference * factor);
	}
}

void TimeTriggerManagerUI::timeTriggerDragged(TimeTriggerUI * ttui, const MouseEvent & e)
{
	if (miniMode) return;

	float diffTime = timeline->getTimeForX(e.getOffsetFromDragStart().x, false);
	float length = ttui->lengthAtMouseDown;
	Point<float> offset(0.f, 0.f);

	if (e.mods.isAltDown() && manager->selectionManager->currentInspectables.size() >= 2)
	{
		ttui->item->scalePosition(Point<float>(diffTime, 0), true);
		scaleSelectedLengths(ttui->item, diffTime);
		return;
	}
	else if (ttui->draggingLength)
	{
		if (e.mods.isShiftDown() || timeline->item->sequence->autoSnap->boolValue())
			length = timeline->item->sequence->getClosestSnapTimeFor(snapTimes, ttui->timeAtMouseDown + length + diffTime) - ttui->timeAtMouseDown;
		else
			length += diffTime;
	}
	else
	{
		offset.y = (e.mods.isAltDown() || e.mods.isCtrlDown() || e.mods.isShiftDown()) ? 0.f : e.getDistanceFromDragStartY() * 1.0f / (getHeight() - 20);
		if (e.mods.isShiftDown() || timeline->item->sequence->autoSnap->boolValue())
		{
			float targetTime = timeline->item->sequence->getClosestSnapTimeFor(snapTimes, ttui->item->movePositionReference.x + diffTime);
			offset.x = targetTime - ttui->item->movePositionReference.x;
		}
		else offset.x = diffTime;
		if (e.mods.isCtrlDown()) length -= offset.x;
	}
	ttui->item->movePosition(offset, true);
	ttui->item->length->setValue(jmax(0.f, length));
}

void TimeTriggerManagerUI::timeTriggerTimeChanged(TimeTriggerUI * ttui)
{
	placeTimeTriggerUI(ttui);
}

void TimeTriggerManagerUI::timeTriggerDeselected(TimeTriggerUI* ttui)
{
	placeTimeTriggerUI(ttui);
}

void TimeTriggerManagerUI::selectionEnded(Array<Component*> selectedComponents)
{
	if(InspectableSelector::getInstanceWithoutCreating()) InspectableSelector::getInstance()->removeSelectorListener(this);
	if (selectedComponents.size() == 0) timeline->item->selectThis();
}
