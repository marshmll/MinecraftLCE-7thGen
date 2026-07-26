#pragma once
using namespace std;

class ClientConstants
{

	// This file holds global constants used by the client.
	// The file should be replaced at compile-time with the
	// proper settings for the given compilation. For example,
	// release builds should replace this file with no-cheat
	// settings.

	// INTERNAL DEVELOPMENT SETTINGS
public:
	static const wstring VERSION_STRING;

	static const bool DEADMAU5_CAMERA_CHEATS = false;

	// Only referenced from SurvivalMode.cpp's ctor (an assert guarding a
	// demo-build-only invariant). This checked-in ClientConstants.h is the
	// non-demo/dev variant (per this header's own "replaced at compile-time"
	// comment) and never defined this flag; added as false (not a demo
	// build) so that dead assert compiles and stays correctly inert.
	static const bool IS_DEMO_VERSION = false;
};