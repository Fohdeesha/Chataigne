/*
  ==============================================================================

	AboutWindow.cpp
	Created: 4 Jan 2018 7:25:58pm
	Author:  Ben

  ==============================================================================
*/

#include "MainIncludes.h"
#include "BuildId.h"

AboutWindow::AboutWindow() :
	Component("About")
{
	aboutImage = ChataigneAssetManager::getInstance()->getAboutImage();
	setSize(aboutImage.getWidth(), aboutImage.getHeight());
}

AboutWindow::~AboutWindow()
{
}

void AboutWindow::paint(Graphics& g)
{
	g.fillAll(BG_COLOR.darker());
	g.drawImage(aboutImage, getLocalBounds().toFloat());

	g.setColour(TEXT_COLOR);
	g.setFont(12);
	g.drawText(getApp().getApplicationName() + " " + getApp().getApplicationVersion(), getLocalBounds().removeFromBottom(30).removeFromRight(200).reduced(5).toFloat(), Justification::right);

	//which commit this binary was built from (see BuildId.h), just above the version
	g.setFont(11);
	g.drawText(String("build ") + CHATAIGNE_BUILD_ID, getLocalBounds().withTrimmedBottom(30).removeFromBottom(18).removeFromRight(260).reduced(5, 0).toFloat(), Justification::right);
}
