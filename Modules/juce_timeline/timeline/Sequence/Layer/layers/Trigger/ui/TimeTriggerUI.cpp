/*
  ==============================================================================

	TimeTriggerUI.cpp
	Created: 10 Dec 2016 11:57:16am
	Author:  Ben

  ==============================================================================
*/

TimeTriggerUI::TimeTriggerUI(TimeTrigger * _tt) :
	BaseItemUI<TimeTrigger>(_tt, Direction::NONE),
	startXOffset(0),
	flagXOffset(0)
{
	labelWidth = 0;
	triggerWidth = 0;
	dragAndDropEnabled = false; //avoid default behavior

	autoDrawContourWhenSelected = false;
	setName(_tt->niceName);

	lockUI.reset(item->isUILocked->createToggle(ImageCache::getFromMemory(OrganicUIBinaryData::padlock_png, OrganicUIBinaryData::padlock_pngSize)));
	addAndMakeVisible(lockUI.get());

	removeBT->setVisible(item->isSelected);
	enabledBT->setVisible(item->isSelected);
	lockUI->setVisible(item->isSelected);
	itemColorUI->setVisible(item->isSelected);

	lengthHandle.setMouseCursor(MouseCursor::LeftRightResizeCursor);
	addAndMakeVisible(&lengthHandle); //added last so it stays on top of the label and the buttons

	updateSizeFromName();

}

TimeTriggerUI::~TimeTriggerUI()
{
}

void TimeTriggerUI::paint(Graphics & g)
{
	Colour c = item->itemColor->getColor();
	if (!item->enabled->boolValue()) c = c.darker(.6f).withAlpha(.7f);
	if (triggerWidth > 0)
	{
		//a bar currently held (entered, not yet exited) is filled brighter, so an active bar reads at a
		//glance even when the flag's outline is off screen
		g.setColour(item->isTriggered->boolValue() ? c.brighter(.4f).withAlpha(.5f) : c.withAlpha(.4f));
		g.fillRect(startXOffset + 1, 0, triggerWidth - 1, getHeight());
	}

	g.setColour(c);
	
	g.fillRect(flagRect);


	if (item->isUILocked->boolValue())
	{
		g.setTiledImageFill(ImageCache::getFromMemory(TimelineBinaryData::smallstripe_png, TimelineBinaryData::smallstripe_pngSize), 0, 0, .1f); 
		g.fillRect(flagRect);
	}

	if (item->isSelected) c = HIGHLIGHT_COLOR;
	else if (item->isPreselected) c = PRESELECT_COLOR;
	else if (item->isTriggered->boolValue()) c = GREEN_COLOR.darker();
	g.setColour(c.brighter());
	g.drawRect(flagRect);
	g.drawVerticalLine(startXOffset, 0, (float)getHeight());
	if (triggerWidth > 0)
		g.drawVerticalLine(startXOffset + triggerWidth, 0, (float)getHeight());

}

void TimeTriggerUI::resized()
{
	Rectangle<int> r = getLocalBounds();

	int ty = (int)(item->flagY->floatValue()*(getHeight() - 20));

	flagRect = r.translated(flagXOffset, ty).withSize(labelWidth, 20);
	lineRect = r.translated(startXOffset, 0).withWidth(6);
	lengthRect = r.translated(startXOffset + triggerWidth - 6, 0).withWidth(8);

	Rectangle<int> p = flagRect.reduced(2, 2);
	if (item->isSelected)
	{

		removeBT->setBounds(p.removeFromRight(p.getHeight()));
		p.removeFromRight(2);
		itemColorUI->setBounds(p.removeFromRight(p.getHeight()).reduced(1));
		enabledBT->setBounds(p.removeFromRight(15));
		p.removeFromRight(2);
		if(lockUI != nullptr) lockUI->setBounds(p.removeFromRight(p.getHeight()));
	}

	itemLabel.setBounds(p);

	//At the bar's end, or straddling a plain flag's start line so a bar can be pulled out of it.
	lengthHandle.setBounds(r.withX(startXOffset + (triggerWidth > 0 ? triggerWidth - 3 : -3)).withWidth(6));
	lengthHandle.setVisible(!item->isUILocked->boolValue());
	lengthHandle.toFront(false);
}

bool TimeTriggerUI::hitTest(int x, int y)
{
	if (flagRect.contains(x, y)) return true;
	if (lineRect.contains(x, y)) return true;
	if (lengthRect.contains(x, y)) return true;
	//JUCE only searches children once the parent itself is hit, so the resize handle has to be in here
	if (lengthHandle.getBounds().contains(x, y)) return true;
	return false;
}

void TimeTriggerUI::updateSizeFromName()
{
	int newWidth = itemLabel.getFont().getStringWidth(itemLabel.getText())+ 15;
	if (item->isSelected)
	{
		newWidth += 60; //for all the buttons
	}
	labelWidth = newWidth;
	triggerUIListeners.call(&TimeTriggerUIListener::timeTriggerTimeChanged, this);
}

void TimeTriggerUI::mouseDown(const MouseEvent& e)
{
	BaseItemUI::mouseDown(e);

	flagYAtMouseDown = item->flagY->floatValue();

	if (item->isUILocked->boolValue()) return;

	item->setMovePositionReference(true);
	//The event may come from the lengthHandle child, in which case getPosition() is relative to it
	draggingLength = e.eventComponent == &lengthHandle || lengthRect.contains(e.getEventRelativeTo(this).getPosition());
	lengthAtMouseDown = item->length->floatValue();
	timeAtMouseDown = item->time->floatValue();

	triggerUIListeners.call(&TimeTriggerUIListener::timeTriggerMouseDown, this, e);
}

void TimeTriggerUI::mouseDrag(const MouseEvent & e)
{
	if (itemLabel.isBeingEdited()) return;

	BaseItemUI::mouseDrag(e);
	
	if (item->isUILocked->boolValue()) return; //After that, nothing will changed if item is locked
	
	if (e.mods.isLeftButtonDown())
	{
		setMouseCursor(MouseCursor::LeftRightResizeCursor);
		triggerUIListeners.call(&TimeTriggerUIListener::timeTriggerDragged, this, e);
	}

/*	if (!e.mods.isCommandDown() && item->selectionManager->currentInspectables.size() == 1)
	{
		float ty = flagYAtMouseDown + e.getOffsetFromDragStart().y * 1.f / (getHeight() - 20);
		item->flagY->setValue(ty);
	}
	*/
}

void TimeTriggerUI::mouseUp(const MouseEvent & e)
{
	BaseItemUI::mouseUp(e);

	if (item->isUILocked->boolValue()) return;
	bool moved = flagYAtMouseDown != item->flagY->floatValue() || timeAtMouseDown != item->time->floatValue();
	bool resized = lengthAtMouseDown != item->length->floatValue();
	if (!moved && !resized) return;

	Array<UndoableAction*> actions;
	if (moved && item->selectionManager->currentInspectables.size() >= 2)
	{
		//addUndoableMoveAction() covers the lengths of the whole selection, so no separate length
		//action here : adding one would give the dragged item two undo entries for one drag.
		item->addMoveToUndoManager(true);
	}
	else
	{
		if (resized) actions.add(item->length->setUndoableValue(lengthAtMouseDown, item->length->floatValue(), true));
		if (moved)
		{
			if (flagYAtMouseDown != item->flagY->floatValue())
				actions.add(item->flagY->setUndoableValue(flagYAtMouseDown, item->flagY->floatValue(), true));
			if (timeAtMouseDown != item->time->floatValue())
				actions.add(item->time->setUndoableValue(timeAtMouseDown, item->time->floatValue(), true));
		}
	}
	if (!actions.isEmpty()) UndoMaster::getInstance()->performActions(resized && !moved ? "Resize Trigger \"" + item->niceName + "\"" : "Move Trigger \"" + item->niceName + "\"", actions);

	//BaseItemMinimalUI does addMouseListener(this, true), so this runs twice per direct event (JUCE
	//documents this). Re-sync the references so the second pass sees no change and pushes nothing.
	flagYAtMouseDown = item->flagY->floatValue();
	timeAtMouseDown = item->time->floatValue();
	lengthAtMouseDown = item->length->floatValue();
	draggingLength = false;
}

void TimeTriggerUI::mouseMove(const MouseEvent& e)
{
	if (e.eventComponent == &lengthHandle) return; //the handle carries its own cursor

	if (item->isUILocked->boolValue()) setMouseCursor(MouseCursor::NormalCursor);
	else if (lengthRect.contains(e.getEventRelativeTo(this).getPosition())) setMouseCursor(MouseCursor::LeftRightResizeCursor);
	else setMouseCursor(MouseCursor::UpDownLeftRightResizeCursor);
}

void TimeTriggerUI::containerChildAddressChangedAsync(ControllableContainer * cc)
{
	BaseItemUI::containerChildAddressChangedAsync(cc);
	updateSizeFromName();
}

void TimeTriggerUI::controllableFeedbackUpdateInternal(Controllable * c)
{
	if (c == item->time || c == item->length)
	{
		triggerUIListeners.call(&TimeTriggerUIListener::timeTriggerTimeChanged, this);
	} else if (c == item->flagY)
	{
		repaint();
		resized();
	} else if (c == item->isTriggered)
	{
		repaint();
	} else if (c == item->isUILocked)
	{
		repaint();
		resized(); //the resize handle is hidden while locked
	}
}

void TimeTriggerUI::inspectableSelectionChanged(Inspectable * i)
{
	BaseItemUI::inspectableSelectionChanged(i);
	removeBT->setVisible(item->isSelected);
	enabledBT->setVisible(item->isSelected);
	if(lockUI != nullptr) lockUI->setVisible(item->isSelected);
	if(itemColorUI != nullptr) itemColorUI->setVisible(item->isSelected);

	updateSizeFromName();

	triggerUIListeners.call(&TimeTriggerUIListener::timeTriggerDeselected, this);
}
