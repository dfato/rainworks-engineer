#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Engine/HostContext.h"

namespace ve::ui
{
    // Live readout of the host transport, one line in the footer. Missing fields
    // show as "--", values we derived from other fields are marked with "*".
    class HostInfoComponent final : public juce::Component
    {
    public:
        void setSnapshot (const HostSnapshot&);
        void paint (juce::Graphics&) override;

    private:
        HostSnapshot snap;
    };
}
