# Soapstorm-Beta Release Notes

Welcome to the latest release of the **Soapstorm** viewer! This update focuses on resolving critical stability issues, rendering errors, and memory leaks, alongside merging the latest improvements from the upstream Firestorm codebase.

---

## Soapstorm Custom Fixes & Features
These are our own improvements and custom patches added to this build:

### Stability & Crash Fixes
* **Fixed Region Teardown Crash**: Resolved a hard crash that occurred during region teardown/disconnection following network timeouts (associated with event poll coroutines).
* **Fixed OpenGL Rendering Error (GL Error 1280)**: Prevented a rendering crash caused by passing unsupported texture targets (such as `GL_TEXTURE_CUBE_MAP` directly) during texture readbacks in `readBackRaw`.
* **Fixed Media Capability Timeout Hangs**: Resolved an issue where empty capability URLs or dropped requests after maximum retries would cause media features to hang or leak.
* **Fixed GPU Texture Memory Leak**: Corrected a leak of GPU texture handles (`mMaskTexName`) and baked layer set caches when avatars were destroyed/recreated.
* **Fixed Sound Blacklist Crash**: Resolved a crash in the sound blacklisting interface.
* **Fixed Sound Decoding Out-of-Memory Crash**: Fixed a background thread crash caused by uncaught memory allocation failures (`std::bad_alloc`) during sound decoding.
* **Fixed Blacklist Null Pointer Crash**: Fixed a potential null pointer crash in the asset blacklist UI when the audio system was not fully initialized.
* **Fixed Mouselook UI Camera Rotation**: Fixed a bug that prevented camera rotation in Over-The-Shoulder (OTS) view when "Show User Interface in Mouselook" was enabled.
* **Fixed Avatar Properties Use-After-Free**: Resolved a use-after-free crash in `LLAvatarPropertiesProcessor` during observer notifications.

### User Interface & Customization
* **Group Panel Layout Fix**: Fixed layout issues where elements were hidden off the bottom of the group panel menu.
* **New Brand Logo**: Replaced the default login logo with custom Soapstorm branding.

### Developer & Platform Enhancements
* **WSL/Linux Build Enhancements**: Enhanced the Linux build scripts for better compatibility with Windows Subsystem for Linux (WSL) and suppressed GCC array-bounds warnings.
* **Cursor Scaling & Warning Fixes**: Implemented cursor scaling support and disabled fatal warnings during Linux compilation.

---

## Merged Upstream Firestorm Changes
We have merged the latest updates and bug fixes from the official Firestorm master branch, including:

* **Localization & Translations**: Updated Polish, French, and Chinese translations.
* **Core Stability**: Consolidated minor rendering, layout, and stability fixes from the main Firestorm branch.
