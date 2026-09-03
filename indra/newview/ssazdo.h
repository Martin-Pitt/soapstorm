/**
 * @file ssazdo.h
 * @brief Soapstorm AZDO (approaching zero driver overhead) settings refresh
 *
 * See doc/azdo_bindless_textures.md
 */

#ifndef LL_SSAZDO_H
#define LL_SSAZDO_H

// re-read the SSBindlessTextures / SSPersistentBuffers / SSMultiDrawIndirect settings and
// push them into the llrender-layer statics. Called at startup from settings_to_globals
// and from the settings change listeners (the bindless one additionally reloads shaders,
// because switching binding modes requires every program to be re-linked).
void ss_azdo_refresh_enabled();

#endif