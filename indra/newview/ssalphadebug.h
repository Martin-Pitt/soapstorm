/**
 * @file ssalphadebug.h
 * @brief TEMPORARY diagnostic - reports why a named prim's face is being drawn in the alpha pool
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#ifndef SS_ALPHADEBUG_H
#define SS_ALPHADEBUG_H

// <SS:Nexii> TEMPORARY. Delete this file, its two CMakeLists entries, and the llappviewer.cpp include and call once the sim-surround transparency question is settled.
// Watches the current selection each frame and reports the moment a selected prim's name matches SSAlphaDebugName, which is the frame the sim's ObjectProperties reply lands. Right-clicking the prim is the whole interaction.
void ssAlphaDebugTick();
// </SS:Nexii>

#endif // SS_ALPHADEBUG_H
