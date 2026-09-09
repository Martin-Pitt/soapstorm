/**
 * @file ssloginavatar.h
 * @brief Login-screen avatar preview: capture the user's avatar on logout and
 *        replay it as a live 3D idle on the login screen, old-school menu style.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#ifndef SS_LOGINAVATAR_H
#define SS_LOGINAVATAR_H

#include "llview.h"
#include "llpointer.h"
#include "llrendertarget.h"

class LLVOAvatar;

// <SS:Nexii> Login-avatar preview: capture-on-logout + offline replay on the login screen.
class SSLoginAvatar
{
public:
    // Serializes the agent avatar (params, baked textures, worn attachment
    // linksets, idle anim assets) into the cache bundle. Safe to call any
    // time the agent avatar is valid and GL is up; no-ops otherwise.
    static void captureOnLogout();

    // Login screen overlay lifecycle (login_panel_holder child, drawn after
    // FSPanelLogin so the avatar sits in front of the login UI).
    static void showOverlay();
    static void hideOverlay();

    // Bundle dir: <cache>/ss_login_avatar
    static std::string getBundleDir();
};

// The overlay view itself. Owns an offscreen avatar rendered into a private
// render target with a transparent background each login frame.
class SSLoginAvatarView : public LLView
{
public:
    SSLoginAvatarView(const LLView::Params& params);
    ~SSLoginAvatarView();

    // LLView
    void draw() override;

private:
    bool buildAvatar();
    void renderPreview();
    void teardown();

    LLPointer<LLVOAvatar> mAvatar;
    LLRenderTarget        mTarget;
    bool                  mBuilt;
    bool                  mFailed;
    F32                   mYaw;
};
// </SS:Nexii>

#endif // SS_LOGINAVATAR_H
