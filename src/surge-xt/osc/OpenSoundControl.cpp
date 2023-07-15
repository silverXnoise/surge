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

#include "OpenSoundControl.h"
#include "Parameter.h"
#include "SurgeSynthProcessor.h"
#include <sstream>
#include <vector>
#include <string>

namespace Surge
{
namespace OSC
{

OpenSoundControl::OpenSoundControl() {}

OpenSoundControl::~OpenSoundControl()
{
    if (listening)
        stopListening();
}

void OpenSoundControl::initOSC(SurgeSynthProcessor *ssp,
                               const std::unique_ptr<SurgeSynthesizer> &surge)
{
    // Init. pointers to synth and synth processor
    synth = surge.get();
    sspPtr = ssp;
}

/* ----- OSC Receiver  ----- */

bool OpenSoundControl::initOSCIn(int port)
{
    if (connect(port))
    {
        addListener(this);
        listening = true;
        iportnum = port;
        synth->storage.oscListenerRunning = true;

#ifdef DEBUG
        std::cout << "SurgeOSC: Listening for OSC on port " << port << "." << std::endl;
#endif
        return true;
    }
    return false;
}

void OpenSoundControl::stopListening()
{
    if (!listening)
        return;

    removeListener(this);
    listening = false;

    if (synth)
        synth->storage.oscListenerRunning = false;

#ifdef DEBUG
    std::cout << "SurgeOSC: Stopped listening for OSC." << std::endl;
#endif
}

// Concatenates OSC message data strings separated by spaces into one string (with spaces)
std::string OpenSoundControl::getWholeString(const juce::OSCMessage &om)
{
    std::string dataStr = "";
    for (int i = 0; i < om.size(); i++)
    {
        dataStr += om[i].getString().toStdString();
        if (i < (om.size() - 1))
            dataStr += " ";
    }
    return dataStr;
}

void OpenSoundControl::oscMessageReceived(const juce::OSCMessage &message)
{
    std::string addr = message.getAddressPattern().toString().toStdString();
    if (addr.at(0) != '/')
        // ignore malformed OSC
        return;

    std::istringstream split(addr);
    // Scan past first '/'
    std::string throwaway;
    std::getline(split, throwaway, '/');

    std::string address1, address2, address3;
    std::getline(split, address1, '/');

    if (address1 == "param")
    {
        auto *p = synth->storage.getPatch().parameterFromOSCName(addr);
        if (p == NULL)
        {
#ifdef DEBUG
            std::cout << "No parameter with OSC address of " << addr << std::endl;
#endif
            // Not a valid OSC address
            return;
        }

        if (!message[0].isFloat32())
        {
            // Not a valid data value
#ifdef DEBUG
            std::cout << "Invalid data type (not float)." << std::endl;
#endif
            return;
        }

        sspPtr->oscRingBuf.push(SurgeSynthProcessor::oscParamMsg(p, message[0].getFloat32()));

#ifdef DEBUG_VERBOSE
        std::cout << "Parameter OSC name:" << p->get_osc_name() << "  ";
        std::cout << "Parameter full name:" << p->get_full_name() << std::endl;
#endif
    }
    else if (address1 == "patch")
    {
        std::getline(split, address2, '/');
        if (address2 == "load")
        {
            std::string dataStr = getWholeString(message) + ".fxp";
            {
                std::lock_guard<std::mutex> mg(synth->patchLoadSpawnMutex);
                strncpy(synth->patchid_file, dataStr.c_str(), FILENAME_MAX);
                synth->has_patchid_file = true;
            }
            synth->processAudioThreadOpsWhenAudioEngineUnavailable();
#ifdef DEBUG
            std::cout << "Patch:" << dataStr << std::endl;
#endif
        }
        else if (address2 == "save")
        {
            // Run this on the juce messenger thread
            juce::MessageManager::getInstance()->callAsync([this, message]() {
                std::string dataStr = getWholeString(message);
                if (dataStr.empty())
                    synth->savePatch(false, true);
                else
                {
                    dataStr += ".fxp";
                    fs::path ppath = fs::path(dataStr);
                    synth->savePatchToPath(ppath);
                }
            });
        }
        else if (address2 == "random")
        {
            synth->selectRandomPatch();
        }
        else if (address2 == "incr")
        {
            synth->jogPatch(true);
        }
        else if (address2 == "decr")
        {
            synth->jogPatch(false);
        }
        else if (address2 == "incr_category")
        {
            synth->jogCategory(true);
        }
        else if (address2 == "decr_category")
        {
            synth->jogCategory(false);
        }
    }

    else if (address1 == "tuning")
    {
        fs::path path = getWholeString(message);
        fs::path def_path;

        std::getline(split, address2, '/');
        // Tuning files path control
        if (address2 == "path")
        {
            std::string dataStr = getWholeString(message);
            if ((dataStr != "_reset") && (!fs::exists(dataStr)))
            {
                std::ostringstream msg;
                msg << "An OSC 'tuning/path/...' message was received with a path which "
                       "does not exist: the default path will not change.";
                synth->storage.reportError(msg.str(), "Path does not exist.");
                return;
            }

            fs::path ppath = fs::path(dataStr);
            std::getline(split, address3, '/');

            if (address3 == "scl")
            {
                if (dataStr == "_reset")
                {
                    ppath = synth->storage.datapath;
                    ppath /= "tuning_library/SCL";
                }
                Surge::Storage::updateUserDefaultPath(&(synth->storage),
                                                      Surge::Storage::LastSCLPath, ppath);
            }
            else if (address3 == "kbm")
            {
                if (dataStr == "_reset")
                {
                    ppath = synth->storage.datapath;
                    ppath /= "tuning_library/KBM Concert Pitch";
                }
                Surge::Storage::updateUserDefaultPath(&(synth->storage),
                                                      Surge::Storage::LastKBMPath, ppath);
            }
        }
        // Tuning file selection
        else if (address2 == "scl")
        {
            if (path.is_relative())
            {
                def_path = Surge::Storage::getUserDefaultPath(
                    &(synth->storage), Surge::Storage::LastSCLPath,
                    synth->storage.datapath / "tuning_library" / "SCL");
                def_path /= path;
                def_path += ".scl";
            }
            else
            {
                def_path = path;
                def_path += ".scl";
            }
#ifdef DEBUG
            std::cout << "scl_path: " << def_path << std::endl;
#endif
            synth->storage.loadTuningFromSCL(def_path);
        }
        // KBM mapping file selection
        else if (address2 == "kbm")
        {
            if (path.is_relative())
            {
                def_path = Surge::Storage::getUserDefaultPath(
                    &(synth->storage), Surge::Storage::LastKBMPath,
                    synth->storage.datapath / "tuning_library" / "KBM Concert Pitch");
                def_path /= path;
                def_path += ".kbm";
            }
            else
            {
                def_path = path;
                def_path += ".kbm";
            }
            synth->storage.loadMappingFromKBM(def_path);
        }
    }
    else if (address1 == "send_all_parameters")
    {
        OpenSoundControl::sendAllParams();
    }
}

void OpenSoundControl::oscBundleReceived(const juce::OSCBundle &bundle)
{
    std::string msg;

#ifdef DEBUG
    std::cout << "OSCListener: Got OSC bundle." << msg << std::endl;
#endif

    for (int i = 0; i < bundle.size(); ++i)
    {
        auto elem = bundle[i];
        if (elem.isMessage())
            oscMessageReceived(elem.getMessage());
        else if (elem.isBundle())
            oscBundleReceived(elem.getBundle());
    }
}

/* ----- OSC Sending  ----- */

bool OpenSoundControl::initOSCOut(int port)
{
    // Send OSC messages to localhost:UDP port number:
    if (!juceOSCSender.connect("127.0.0.1", port))
    {
#ifdef DEBUG
        std::cout << "Surge OSCSender: failed to connect to UDP port " << port << "." << std::endl;
#endif
        return false;
    }
    sendingOSC = true;
    oportnum = port;
    synth->storage.oscSending = true;

#ifdef DEBUG
    std::cout << "SurgeOSC: Sending OSC on port " << port << "." << std::endl;
#endif
    return true;
}

void OpenSoundControl::stopSending()
{
    if (!sendingOSC)
        return;

    sendingOSC = false;
    synth->storage.oscSending = false;
#ifdef DEBUG
    std::cout << "SurgeOSC: Stopped sending OSC." << std::endl;
#endif
}

void OpenSoundControl::send(std::string addr, std::string msg)
{
    if (sendingOSC)
    {
        // Runs on the juce messenger thread
        juce::MessageManager::getInstance()->callAsync([this, msg, addr]() {
            if (!this->juceOSCSender.send(juce::OSCMessage(juce::String(addr), juce::String(msg))))
                std::cout << "Error: could not send OSC message.";
        });
    }
}

// Loop through all params, send them to OSC Out
void OpenSoundControl::sendAllParams()
{
    if (sendingOSC)
    {
        // Runs on the juce messenger thread
        juce::MessageManager::getInstance()->callAsync([this]() {
            // auto timer = new Surge::Debug::TimeBlock("ParameterDump");
            std::string valStr;
            int n = synth->storage.getPatch().param_ptr.size();
            for (int i = 0; i < n; i++)
            {
                Parameter p = *synth->storage.getPatch().param_ptr[i];
                switch (p.valtype)
                {
                case vt_int:
                    valStr = std::to_string(p.val.i);
                    break;

                case vt_bool:
                    valStr = std::to_string(p.val.b);
                    break;

                case vt_float:
                    valStr = std::to_string(p.val.f);
                    break;

                default:
                    break;
                }

                if (!this->juceOSCSender.send(
                        juce::OSCMessage(juce::String(p.oscName), juce::String(valStr))))
                    std::cout << "Error: could not send OSC message.";
            }
            // delete timer;    // This prints the elapsed time
        });
    }
}

} // namespace OSC
} // namespace Surge