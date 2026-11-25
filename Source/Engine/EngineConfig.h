#pragma once

// Engine-wide constants. Everything under Source/Engine must stay free of
// juce_gui_* includes so it can be reused headless (tests, offline render,
// a future inference sidecar). CMake builds the engine tests without any GUI
// module, which enforces this.

namespace ve
{
    namespace Bus
    {
        // Input bus indices as declared in VocalEngineProcessor.
        inline constexpr int mainInput  = 0;
        inline constexpr int sidechain  = 1;
        // Output bus index.
        inline constexpr int mainOutput = 0;
    }

    inline constexpr int maxMainChannels      = 2; // mono or stereo vocal
    inline constexpr int sidechainChannels    = 2; // stereo instrumental bus

    // Sidechain is treated as "receiving signal" above this peak (about -90 dBFS).
    inline constexpr float sidechainSignalFloor = 3.0e-5f;
}
