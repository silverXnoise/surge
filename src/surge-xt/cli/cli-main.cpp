/*
 * Surge XT - a free and open source hybrid synthesizer,
 * built by Surge Synth Team
 *
 * Learn more at https://surge-synthesizer.github.io/
 *
 * Copyright 2018-2023, various authors, as described in the GitHub
 * transaction log.
 *
 * Surge XT is released under the GNU General Public Licence v3
 * or later (GPL-3.0-or-later). The license is found in the "LICENSE"
 * file in the root of this repository, or at
 * https://www.gnu.org/licenses/gpl-3.0.en.html
 *
 * Surge was a commercial product from 2004-2018, copyright and ownership
 * held by Claes Johanson at Vember Audio during that period.
 * Claes made Surge open source in September 2018.
 *
 * All source for Surge XT is available at
 * https://github.com/surge-synthesizer/surge
 */

#include <iostream>

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <CLI11/CLI11.hpp>

#include "version.h"

#include "SurgeSynthProcessor.h"

// Thanks
// https://stackoverflow.com/questions/16077299/how-to-print-current-time-with-milliseconds-using-c-c11
std::string logTimestamp()
{
    using namespace std::chrono;
    using clock = system_clock;

    const auto current_time_point{clock::now()};
    const auto current_time{clock::to_time_t(current_time_point)};
    const auto current_localtime{*std::localtime(&current_time)};
    const auto current_time_since_epoch{current_time_point.time_since_epoch()};
    const auto current_milliseconds{duration_cast<milliseconds>(current_time_since_epoch).count() %
                                    1000};

    std::ostringstream stream;
    stream << std::put_time(&current_localtime, "%T") << "." << std::setw(3) << std::setfill('0')
           << current_milliseconds;
    return stream.str();
}

enum LogLevels
{
    NONE = 0,
    BASIC = 1,
    VERBOSE = 2
};
int logLevel{BASIC};
#define LOG(lev, x)                                                                                \
    {                                                                                              \
        if (lev >= logLevel)                                                                       \
        {                                                                                          \
            std::cout << logTimestamp() << " - " << x << std::endl;                                \
        }                                                                                          \
    }
#define PRINT(x) LOG(logLevel + 1, x);
#define PRINTERR(x) LOG(logLevel + 1, "Error: " << x);

void listAudioDevices()
{
    juce::AudioDeviceManager manager;

    juce::OwnedArray<juce::AudioIODeviceType> types;
    manager.createAudioDeviceTypes(types);

    for (int i = 0; i < types.size(); ++i)
    {
        juce::String typeName(types[i]->getTypeName());

        types[i]->scanForDevices(); // This must be called before getting the list of devices

        juce::StringArray deviceNames(types[i]->getDeviceNames()); // This will now return a list of
                                                                   // available devices of this type

        for (int j = 0; j < deviceNames.size(); ++j)
        {
            PRINT("Audio Device: [" << i << "." << j << "] : " << typeName << "."
                                    << deviceNames[j]);
        }
    }
}

void listMidiDevices()
{
    juce::MessageManager::getInstance()->setCurrentThreadAsMessageThread();

    auto items = juce::MidiInput::getAvailableDevices();
    for (int i = 0; i < items.size(); ++i)
    {
        PRINT("MIDI Device: [" << i << "] : " << items[i].name);
    }
    juce::MessageManager::deleteInstance();
}

struct SurgePlayback : juce::MidiInputCallback, juce::AudioIODeviceCallback
{
    std::unique_ptr<SurgeSynthProcessor> proc;
    SurgePlayback() { proc = std::make_unique<SurgeSynthProcessor>(); }

    static constexpr int midiBufferSz{4096}, midiBufferSzMask{midiBufferSz - 1};
    std::array<juce::MidiMessage, midiBufferSz> midiBuffer;
    std::atomic<int> midiWP{0}, midiRP{0};
    void handleIncomingMidiMessage(juce::MidiInput *source,
                                   const juce::MidiMessage &message) override
    {
        midiBuffer[midiWP] = message;
        midiWP = (midiWP + 1) & midiBufferSzMask;
    }

    int pos = BLOCK_SIZE;
    void
    audioDeviceIOCallbackWithContext(const float *const *inputChannelData, int numInputChannels,
                                     float *const *outputChannelData, int numOutputChannels,
                                     int numSamples,
                                     const juce::AudioIODeviceCallbackContext &context) override
    {
        proc->processBlockOSC();

        for (int i = 0; i < numSamples; ++i)
        {
            if (pos >= BLOCK_SIZE)
            {
                int mw = midiWP;

                while (mw != midiRP)
                {
                    proc->applyMidi(midiBuffer[midiRP]);
                    midiRP = (midiRP + 1) & midiBufferSzMask;
                }
                proc->surge->process();
                pos = 0;
            }
            outputChannelData[0][i] = proc->surge->output[0][pos];
            outputChannelData[1][i] = proc->surge->output[1][pos];
            pos++;
        }
    }

    void audioDeviceStopped() override {}
    void audioDeviceAboutToStart(juce::AudioIODevice *device) override
    {
        LOG(BASIC, "Audio Starting      : SampleRate=" << device->getCurrentSampleRate()
                                                       << " BufferSize="
                                                       << device->getCurrentBufferSizeSamples());
        proc->surge->setSamplerate(device->getCurrentSampleRate());
    }
};

int main(int argc, char **argv)
{
    // juce::ConsoleApplication is just such a mess.
    CLI::App app("surge-xt-CLI : a command line player for surge-xt");

    app.set_version_flag("--version", Surge::Build::FullVersionStr);

    bool listDevices{false};
    app.add_flag("-l,--list-devices", listDevices,
                 "List all devices available on this system, then exit");

    std::string audioInterface{};
    app.add_flag("-a,--audio-interface", audioInterface,
                 "Select an audio interface, using index (like '0.2') as shown in list-devices");

    int midiInput{0};
    app.add_flag("-m,--midi-input", midiInput,
                 "Select an midi input using the index from list-devices");

    int oscInputPort{0};
    app.add_flag("--osc-in-port", oscInputPort, "Port for OSC Input; unspecified means no OSC");

    int oscOutputPort{0};
    app.add_flag("--osc-out-port", oscOutputPort,
                 "Port for OSC Output; unspecified means input only; input required");

    std::string initPatch{};
    app.add_flag("--init-patch", initPatch, "Choose this file (by path) as the initial patch");

    CLI11_PARSE(app, argc, argv);

    if (listDevices)
    {
        listAudioDevices();
        listMidiDevices();
        exit(0);
    }

    /*
     * This is the default runloop. Basically this main thread acts as the message queue
     */
    auto engine = std::make_unique<SurgePlayback>();
    if (!initPatch.empty())
    {
        engine->proc->surge->loadPatchByPath(initPatch.c_str(), -1, "Loaded Patch");
    }

    auto *mm = juce::MessageManager::getInstance();
    mm->setCurrentThreadAsMessageThread();

    auto items = juce::MidiInput::getAvailableDevices();
    if (midiInput < 0 || midiInput >= items.size())
    {
        PRINTERR("Midi Input must be in range 0..." << items.size() - 1);
        exit(5);
    }
    auto vmini = items[midiInput];

    auto inp = juce::MidiInput::openDevice(vmini.identifier, engine.get());
    if (!inp)
    {
        PRINTERR("Unable to open midi device " << vmini.name);
        exit(1);
    }
    LOG(BASIC, "Opened Midi Input   : [" << vmini.name << "] ");

    juce::AudioDeviceManager manager;

    juce::OwnedArray<juce::AudioIODeviceType> types;
    manager.createAudioDeviceTypes(types);

    int audioTypeIndex = 0;
    int audioDeviceIndex = 0;
    if (audioInterface.empty())
    {
        types[audioTypeIndex]->scanForDevices();
        audioDeviceIndex = types[audioTypeIndex]->getDefaultDeviceIndex(false);
        LOG(BASIC, "Audio device is unspecified: Using system default");
    }
    else
    {
        auto p = audioInterface.find('.');
        if (p == std::string::npos)
        {
            PRINTERR("Audio Interface Argument must be of form a.b, per --list-devices. You gave "
                     << audioInterface);
            exit(3);
        }
        else
        {
            auto da = std::atoi(audioInterface.substr(0, p).c_str());
            auto dt = std::atoi(audioInterface.substr(p + 1).c_str());
            audioTypeIndex = da;
            audioDeviceIndex = dt;

            if (da < 0 || da >= types.size())
            {
                PRINTERR("Audio Type Index must be in range 0..." << types.size() - 1);
                exit(4);
            }
        }
    }

    const auto &atype = types[audioTypeIndex];
    LOG(BASIC, "Audio Driver Type   : [" << atype->getTypeName() << "]")

    atype->scanForDevices(); // This must be called before getting the list of devices
    juce::StringArray deviceNames(atype->getDeviceNames()); // This will now return a list of

    if (audioDeviceIndex < 0 || audioDeviceIndex >= deviceNames.size())
    {
        PRINTERR("Audio Device Index must be in range 0..." << deviceNames.size() - 1);
    }

    const auto &dname = deviceNames[audioDeviceIndex];
    auto device = atype->createDevice(dname, "");
    if (!device)
    {
        PRINTERR("Unable to open audio output " << dname);
        exit(2);
    }
    LOG(BASIC, "Audio Output        : [" << device->getName() << "]");

    auto res = device->open(0, 3 /* bitset - careful */, 48000, 256);
    if (!res.isEmpty())
    {
        PRINTERR("Unable to open audio device: " << res);
        exit(3);
    }

    device->start(engine.get());
    inp->start();

    bool needsMessageLoop{false};
    if (oscInputPort > 0)
    {
        needsMessageLoop = true;
        LOG(BASIC, "Starting OSC Input on " << oscInputPort);
        engine->proc->initOSCIn(oscInputPort);
        if (oscOutputPort > 0)
        {
            LOG(BASIC, "Starting OSC Output on " << oscOutputPort);
            engine->proc->initOSCOut(oscOutputPort);
        }
    }

    if (needsMessageLoop)
    {
        LOG(BASIC, "Beginning message loop");
    }
    else
    {
        LOG(BASIC, "Running");
    }

    // TODO: What's the stopping condition
    while (true)
    {
        if (needsMessageLoop)
        {
            using namespace std::chrono_literals;
            std::this_thread::sleep_for(25ms);
            mm->runDispatchLoop();
        }
        else
        {
            using namespace std::chrono_literals;
            std::this_thread::sleep_for(1000ms);
        }
    }

    // Handle interrupt and collect these in lambda to close if you bail out
    device->stop();
    device->close();
    inp->stop();
    juce::MessageManager::deleteInstance();
}