#pragma once
#include "Screen.h"
using namespace std;

class ChatScreen : public Screen
{
protected:
	wstring message;
private:
	int frame;

public:
	ChatScreen();	//4J added
	virtual void init();
    virtual void removed();
    virtual void tick();
private:
	// Lazily-initialized (see ChatScreen.cpp) rather than a plain static
	// object copied from SharedConstants::acceptableLetters at static-init
	// time: that's a different translation unit's global, and C++ doesn't
	// guarantee cross-TU dynamic-initialization order - under this port's
	// link order it ran before SharedConstants::acceptableLetters' own
	// constructor, reading unconstructed memory and crashing. A
	// function-local static defers the copy to first real use, by which
	// point SharedConstants::staticCtor() has long since populated it.
	static const wstring &getAllowedChars();
protected:
	void keyPressed(wchar_t ch, int eventKey);
public:
	void render(int xm, int ym, float a);
protected:
	void mouseClicked(int x, int y, int buttonNum);
};