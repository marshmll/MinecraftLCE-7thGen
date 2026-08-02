#pragma once

#include "Common/Leaderboards/LeaderboardManager.h"

// Linux has no online service behind it, so this is an offline no-op exactly like
// Windows64/Leaderboards/WindowsLeaderboardManager.h - the file this mirrors.
//
// It exists because LeaderboardManager is abstract and its m_instance singleton is
// deliberately defined once per platform (see the .cpp). Without a concrete subclass
// Instance() returns NULL, and none of the ~20 call sites null-check it:
// UIScene_LeaderboardsMenu's constructor and StatsCounter::flushLeaderboards() /
// saveLeaderboards() all dereference it straight away.
//
// OpenSession() returns true so StatsCounter proceeds to writeStats() (which then
// no-ops in WriteStats) rather than logging a failure every save - same as Windows64.
class LinuxLeaderboardManager : public LeaderboardManager
{
public:
	virtual void Tick() {}

	//Open a session
	virtual bool OpenSession() { return true; }

	//Close a session
	virtual void CloseSession() {}

	//Delete a session
	virtual void DeleteSession() {}

	//Write the given stats
	//This is called synchronously and will not free any memory allocated for views when it is done
	virtual bool WriteStats(unsigned int viewCount, ViewIn views) { return false; }

	virtual bool ReadStats_Friends(LeaderboardReadListener *callback, int difficulty, EStatsType type, PlayerUID myUID) { return false; }
	virtual bool ReadStats_MyScore(LeaderboardReadListener *callback, int difficulty, EStatsType type, PlayerUID myUID, unsigned int readCount) { return false; }
	virtual bool ReadStats_TopRank(LeaderboardReadListener *callback, int difficulty, EStatsType type, unsigned int startIndex, unsigned int readCount) { return false; }

	//Perform a flush of the stats
	virtual void FlushStats() {}

	//Cancel the current operation
	virtual void CancelOperation() {}

	//Is the leaderboard manager idle.
	virtual bool isIdle() { return true; }
};
