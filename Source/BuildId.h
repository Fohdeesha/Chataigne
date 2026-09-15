#pragma once

//Build identity : the git commit this binary was built from (short hash, "+" when the tree had uncommitted changes)
//and that commit's date. Shown in the About window and logged at startup, so a running instance can be matched to a
//commit without touching the version string, which the Welcome window, the file-format migration, every saved file
//and the update checker all key on.
//BuildInfo.h is written by tools/build/stamp_build.py before every build and is not committed.
#if __has_include("BuildInfo.h")
#include "BuildInfo.h"
#else
#define CHATAIGNE_BUILD_ID "unstamped build"
#endif
