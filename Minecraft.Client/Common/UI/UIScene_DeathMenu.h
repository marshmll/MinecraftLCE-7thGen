#pragma once

#include "UIScene.h"
#include "IUIScene_PauseMenu.h"

// Derives from IUIScene_PauseMenu because it reuses that interface's exit-dialog
// callbacks (ExitGameDialogReturned / ExitGameSaveDialogReturned), and those cast the
// pParam they are handed straight to IUIScene_PauseMenu*. Previously this class passed a
// raw `this` while not deriving from the interface at all, so the callbacks reinterpreted
// an unrelated type and their scene->SetIgnoreInput(true) call dispatched through whatever
// sat in that vtable slot - UIScene::reloadMovie(bool). Same defect as UIScene_PauseMenu's,
// but worse, because there the two types were at least related by inheritance.
class UIScene_DeathMenu : public UIScene, public IUIScene_PauseMenu
{
private:
	enum EControls
	{
		eControl_Respawn,
		eControl_ExitGame
	};

	bool m_bIgnoreInput;

	UIControl_Button m_buttonRespawn, m_buttonExitGame;
	UIControl_Label m_labelTitle;
	UI_BEGIN_MAP_ELEMENTS_AND_NAMES(UIScene)
		UI_MAP_ELEMENT( m_buttonRespawn, "Respawn")
		UI_MAP_ELEMENT( m_buttonExitGame, "ExitGame")
		UI_MAP_ELEMENT( m_labelTitle, "Title")
	UI_END_MAP_ELEMENTS_AND_NAMES()
public:
	UIScene_DeathMenu(int iPad, void *initData, UILayer *parentLayer);
	virtual ~UIScene_DeathMenu();

	virtual EUIScene getSceneType() { return eUIScene_DeathMenu;}
	virtual void updateTooltips();

protected:
	// TODO: This should be pure virtual in this class
	virtual wstring getMoviePath();

public:
	// INPUT
	virtual void handleInput(int iPad, int key, bool repeat, bool pressed, bool released, bool &handled);

protected:
	void handlePress(F64 controlId, F64 childId);

	// IUIScene_PauseMenu's two pure virtuals. SetIgnoreInput is what the exit-dialog
	// callbacks invoke, and this scene already had the m_bIgnoreInput flag it should be
	// driving (handleInput checks it) - it just had no way to be set.
	virtual void ShowScene(bool show);
	virtual void SetIgnoreInput(bool ignoreInput);

#ifdef _DURANGO	
	virtual long long getDefaultGtcButtons() { return 0; }
#endif
};
