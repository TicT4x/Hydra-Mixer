#include "RoomCorrectionPlugin.h"
#include "RoomCorrectionEditor.h"
#include "ui/Theme.h"

namespace hydra
{
RoomCorrectionProcessor::RoomCorrectionProcessor()
    : juce::AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                             .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    addParameter (onParam = new juce::AudioParameterBool (juce::ParameterID { "on", 1 }, "Correction On", true));
    correction.on = &on;
    startTimerHz (2);
}

RoomCorrectionProcessor::~RoomCorrectionProcessor() { stopTimer(); }

juce::Colour RoomCorrectionProcessor::colour() const { return theme::accentIn; }

bool RoomCorrectionProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo()) && layouts.getMainInputChannelSet() == out;
}

void RoomCorrectionProcessor::prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock)
{
    rate.store (sampleRate);
    scratch.assign ((size_t) juce::jmax (512, maximumExpectedSamplesPerBlock), 0.0f);
    correction.reset();
    // The coefficients for this rate (message thread; else the timer does it, until then the signal passes dry).
    if (juce::MessageManager::existsAndIsCurrentThread()) correction.update (sampleRate, blocks.load(), true);
}

void RoomCorrectionProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    inProcess.store (true);   // before the coefficients are read (see timerCallback)
    on.set (onParam->get() ? 1.0f : 0.0f);
    const double sr = rate.load (std::memory_order_relaxed);
    const int n = buffer.getNumSamples();
    if (buffer.getNumChannels() >= 2)
        correction.process (buffer.getWritePointer (0), buffer.getWritePointer (1), n, sr);
    else if (buffer.getNumChannels() == 1 && ! scratch.empty())
    {
        // Mono: the left channel's filters; the copy carries the right one along.
        auto* x = buffer.getWritePointer (0);
        for (int i = 0; i < n; i += (int) scratch.size())
        {
            const int m = juce::jmin ((int) scratch.size(), n - i);
            std::copy (x + i, x + i + m, scratch.begin());
            correction.process (x + i, scratch.data(), m, sr);
        }
    }
    blocks.fetch_add (1, std::memory_order_release);
    inProcess.store (false, std::memory_order_release);
}

void RoomCorrectionProcessor::setProfile (CorrectionProfile p)
{
    correction.setProfile (std::move (p), rate.load(), blocks.load());
}

void RoomCorrectionProcessor::setOn (bool b)
{
    onParam->beginChangeGesture();
    onParam->setValueNotifyingHost (b ? 1.0f : 0.0f);
    onParam->endChangeGesture();
}

void RoomCorrectionProcessor::timerCallback()
{
    {
        std::optional<CorrectionProfile> p;
        {
            const juce::ScopedLock sl (pendingLock);
            p.swap (pending);
        }
        if (p.has_value())
        {
            setProfile (std::move (*p));
            link = pool::Link (pool::kPlugin);   // a loaded state: the pool's version of its entry wins
        }
    }
    link.sync (*this);
    // A rate change rebuilds the coefficients; sets the audio thread has let go of are freed.
    correction.update (rate.load(), blocks.load (std::memory_order_acquire), inProcess.load (std::memory_order_acquire));
}

//==============================================================================
void RoomCorrectionProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    juce::XmlElement xml ("HYDRAROOMCORRECTION");
    xml.setAttribute ("version", 1);
    xml.setAttribute ("on", onParam->get());
    xml.setAttribute ("editorW", editorSize.x);
    xml.setAttribute ("editorH", editorSize.y);
    // The copy: the project sounds the same without the pool (another computer).
    if (! getProfile().isEmpty()) xml.addChildElement (getProfile().toXml().release());
    copyXmlToBinary (xml, dest);
}

void RoomCorrectionProcessor::setStateInformation (const void* data, int size)
{
    auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr || ! xml->hasTagName ("HYDRAROOMCORRECTION")) return;
    *onParam = xml->getBoolAttribute ("on", true);
    editorSize = { juce::jlimit (900, 3000, xml->getIntAttribute ("editorW", editorSize.x)),
                   juce::jlimit (600, 2000, xml->getIntAttribute ("editorH", editorSize.y)) };
    auto p = CorrectionProfile::fromXml (xml->getChildByName ("CORRECTION"));
    {
        const juce::ScopedLock sl (pendingLock);
        pending = std::move (p);
    }
    if (juce::MessageManager::existsAndIsCurrentThread()) timerCallback();
}

juce::AudioProcessorEditor* RoomCorrectionProcessor::createEditor()
{
    return new RoomCorrectionEditor (*this);
}
} // namespace hydra

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new hydra::RoomCorrectionProcessor();
}
