#pragma once

#include "StyleProfile.h"
#include <juce_audio_basics/juce_audio_basics.h>

namespace ve::ToneShaper
{
    // Colours a rendered take the way a genre's singers and their engineers usually do:
    // brightness (presence + air shelf), warmth (low-mid shelf), grit (saturation of the
    // mids that tracks the voice's level, so quiet words get rasp too), breath (airy noise
    // that follows the voice) and compression. The take's overall loudness is kept.
    // A style with no tone settings leaves the audio untouched.
    void process (juce::AudioBuffer<float>&, double sampleRate, const StyleProfile&);
}
