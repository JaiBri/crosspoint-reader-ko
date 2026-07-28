#pragma once

class GfxRenderer;

// Reload custom reader font - removes old font and loads new one
// Call this when font settings change to apply immediately without reboot
// Returns true if custom font was loaded successfully
bool reloadCustomReaderFont();

// Reload UI system font - removes old SD system font and loads the configured one,
// making it the primary UI font with Pretendard as its glyph-level fallback. If the
// setting was cleared, the UI reverts to Pretendard.
// Call this when the system-font setting changes to apply immediately without reboot.
// Returns true if a system font is now active.
bool reloadSystemFont();

// Get reference to global renderer (for font operations from other modules)
GfxRenderer& getGlobalRenderer();
