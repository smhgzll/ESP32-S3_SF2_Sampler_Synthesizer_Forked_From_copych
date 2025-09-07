#pragma once
#ifdef ENABLE_GUI

#include "config.h"
#include "TextGUI.h"
#include <FS.h>
#include <SdFat.h>
#include <LittleFS.h>
#include "synth.h"
#include "TLVStorage.h"
#include "fx_reverb.h"
#include "fx_delay.h"
#include "fx_chorus.h"

extern SdFs SD;

namespace MenuStructure {

static std::vector<MenuItem> withFallback(std::vector<MenuItem> items) {
    if (items.empty()) {
        items.push_back(MenuItem::Action("(empty)", [](TextGUI&) { }));
    }
    return items;
}

inline bool endsWithIgnoreCase(const String& str, const String& suffix) {
    if (str.length() < suffix.length()) return false;
    return str.substring(str.length() - suffix.length()).equalsIgnoreCase(suffix);
}

inline bool folderContainsSf2(fs::FS& fs, const String& path) {
    File dir = fs.open(path);
    if (!dir || !dir.isDirectory()) return false;

    File entry;
    while ((entry = dir.openNextFile())) {
        String name = entry.name();
        if (entry.isDirectory()) {
            if (folderContainsSf2(fs, name)) return true;  // recurse
        } else if (endsWithIgnoreCase(name, ".sf2")) {
            return true;
        }
    }
    return false;
}

inline bool folderContainsSf2SdFat(const String& path) {
    FsFile dir;
    if (!dir.open(path.c_str())) return false;

    FsFile entry;
    char nm[128];
    while (entry.openNext(&dir, O_RDONLY)) {
        entry.getName(nm, sizeof(nm));
        String entryName = String(nm);

        String fullPath = entryName.startsWith("/") ? entryName : (String(path) + "/" + entryName);
        fullPath.replace("//", "/");

        if (entry.isDir()) {
            entry.close();
            if (folderContainsSf2SdFat(fullPath)) { dir.close(); return true; }
        } else {
            bool ok = endsWithIgnoreCase(entryName, ".sf2");
            entry.close();
            if (ok) { dir.close(); return true; }
        }
    }
    dir.close();
    return false;
}

static MenuItem createFileBrowserMenuSdFat(Synth& synth, const String& path, FileSystemType type, const String& label) {
    return MenuItem::Submenu("▶" + label, [=, &synth]() {
        std::vector<MenuItem> items;

        FsFile dir;
        if (!dir.open(path.c_str())) return items;

        FsFile entry;
        char nm[128];
        while (entry.openNext(&dir, O_RDONLY)) {
            entry.getName(nm, sizeof(nm));
            String entryName = String(nm);

            String fullPath = entryName.startsWith("/") ? entryName : (String(path) + "/" + entryName);
            fullPath.replace("//", "/");

            if (entry.isDir()) {
                entry.close();
                if (folderContainsSf2SdFat(fullPath)) {
                    String lbl = fullPath.substring(fullPath.lastIndexOf("/") + 1);
                    items.push_back(createFileBrowserMenuSdFat(synth, fullPath, type, lbl));
                }
            } else if (endsWithIgnoreCase(entryName, ".sf2")) {
                items.push_back(MenuItem::Action(entryName, [=, &synth](TextGUI& gui) {
                    synth.setFileSystem(type);
                    gui.busyMessage("Loading...");
                    synth.loadSf2File(fullPath.c_str());
                }));
                entry.close();
            } else {
                entry.close();
            }
        }
        dir.close();
        return withFallback(items);
    });
}

static MenuItem createFileBrowserMenu(Synth& synth, fs::FS& fs, const String& path, FileSystemType type, const String& label) {
    return MenuItem::Submenu("▶" + label, [=, &synth, &fs]() {
        std::vector<MenuItem> items;
        File dir = fs.open(path);

        if (!dir || !dir.isDirectory()) return items;

        File entry;
        while ((entry = dir.openNextFile())) {
            String entryName = entry.name();

            // Normalize path to full absolute path
            String fullPath = entryName;
            if (!entryName.startsWith("/")) {
                fullPath = path + "/" + entryName;
            }

            // Sanitize (avoid "//")
            fullPath.replace("//", "/");

            if (entry.isDirectory()) {
                if (folderContainsSf2(fs, fullPath)) {
                    String label = fullPath.substring(fullPath.lastIndexOf("/") + 1);
                    items.push_back(createFileBrowserMenu(synth, fs, fullPath, type, label));
                }
            } else if (endsWithIgnoreCase(entryName, ".sf2")) {
                items.push_back(MenuItem::Action(entryName, [=, &synth](TextGUI& gui) {
                    synth.setFileSystem(type);
                    gui.busyMessage( "Loading...");
                    synth.loadSf2File(fullPath.c_str());
                }));
            }
        }
        return withFallback(items);
    });
}

static MenuItem createProgramMenu(Synth& synth, uint8_t channel) {
    struct ProgramEntry {
        uint16_t bank;
        uint8_t program;
        String name;
    };

    auto& parser = synth.parser;

    std::map<uint8_t, std::vector<ProgramEntry>> melodic, sfx, sfxkits, drums;

    for (const auto& p : parser.getPresets()) {
        ProgramEntry entry{ p.bank, (uint8_t)p.program, p.name };
        uint8_t msb = p.bank >> 7;

        if (msb == 0x7F || p.bank == 128)   drums[p.program].push_back(entry);
        else if (msb == 0x7E)               sfxkits[p.program].push_back(entry);
        else if (msb == 0x40)               sfx[p.program].push_back(entry);
        else                                melodic[p.program].push_back(entry);
    }

    return MenuItem::Submenu("Program", [=, &synth]() {
        uint8_t ch = channel;  // capture as local value

        auto makeGroup = [&](const char* title, const auto& group) -> MenuItem {
            return MenuItem::Submenu(title, [=, &synth]() {
                std::vector<MenuItem> items;
                for (const auto& [program, variants] : group) {
                    if (variants.size() == 1) {
                        const auto& e = variants[0];
                        auto& chRef = synth.channels[ch];

                        bool isActive = (chRef.program == e.program) && (chRef.getBank() == e.bank);
                        String label = String(e.program) + ": " + e.name + (isActive ? " ●" : "");

                        items.emplace_back(MenuItem::Action(label, [=, &synth](TextGUI&) {
                            auto& chRef = synth.channels[ch];
                            chRef.wantProgram = e.program;
                            chRef.wantBankMSB = (e.bank >> 7) & 0x7F;
                            chRef.wantBankLSB = e.bank & 0x7F;
                            synth.applyBankProgram(ch);
                        }));
                    } else {
                        String baseLabel = String(program) + ": " + variants[0].name;

                        items.emplace_back(MenuItem::Submenu(baseLabel, [=, &synth]() {
                            std::vector<MenuItem> subitems;
                            for (const auto& e : variants) {
                                auto& chRef = synth.channels[ch];
                                bool isActive = (chRef.program == e.program) && (chRef.getBank() == e.bank);
                                String label = "Bank " + String(e.bank) + (isActive ? " ✔" : "");

                                subitems.emplace_back(MenuItem::Action(label, [=, &synth](TextGUI&) {
                                    auto& chRef = synth.channels[ch];
                                    chRef.wantProgram = e.program;
                                    chRef.wantBankMSB = (e.bank >> 7) & 0x7F;
                                    chRef.wantBankLSB = e.bank & 0x7F;
                                    synth.applyBankProgram(ch);
                                }));
                            }
                            return subitems;
                        }));
                    }
                }
                return withFallback(items);
            });
        };

        return std::vector<MenuItem>{
            makeGroup("Melodic", melodic),
            makeGroup("SFX", sfx),
            makeGroup("SFX kits", sfxkits),
            makeGroup("Drums", drums)
        };
    });
}


static MenuItem createLoadBankMenu(Synth& synth) {
    return MenuItem::Submenu("Load Bank", [=, &synth]() {
        return std::vector<MenuItem>{
            // SD (SdFat)
            createFileBrowserMenuSdFat(synth, "/", FileSystemType::SD, "SD Card"),
            // Internal (LittleFS)
            createFileBrowserMenu(synth, LittleFS, "/", FileSystemType::LITTLEFS, "Internal")
        };
    });
}

#ifdef ENABLE_REVERB
// Reverb Menu (matches your FxReverb implementation)
std::vector<MenuItem> createReverbMenu(SynthState& state) {
    return {
        MenuItem::Toggle("Enabled",
            [&state]() { return state.reverb.getLevel() > 0.0f; },
            [&state](int v) { state.reverb.setLevel(v ? 1.0f : 0.0f); }),
            
        MenuItem::Value("Level",
            [&state]() { return (int)(state.reverb.getLevel() * 100.0f); },
            [&state](int v) { state.reverb.setLevel(((float)v + 0.5f) / 100.0f); }, 
            0 , 100 , 1  ),
            
        MenuItem::Value("Time",
            [&state]() { return (int)(state.reverb.getTimeRaw() * 100.0f); }, 
            [&state](int v) { state.reverb.setTime( ((float)v + 0.5f) / 100.0f ); },
            1, 100, 1), 
            
        MenuItem::Value("Damping",
            [&state]() { return (int)(state.reverb.getDamping() * 100); },
            [&state](int v) { state.reverb.setDamping(((float)v + 0.5f) / 100.0f); },
            1, 100, 1), 
            
        MenuItem::Value("Pre-delay",
            [&state]() { return (int)state.reverb.getPreDelayTime(); }, // ms
            [&state](int v) { state.reverb.setPreDelayTime(((float)v + 0.5f)); },
            0, MAX_PREDELAY_MS, 5) 
    };
}
#endif

#ifdef ENABLE_CHORUS
// Chorus Menu (matches FxChorus)
std::vector<MenuItem> createChorusMenu(SynthState& state) {
    return {
        MenuItem::Toggle("Enabled",
            [&state]() { return state.chorus.getDepth() > 0.0f; },
            [&state](int v) { state.chorus.setDepth(v ? 0.002f : 0.0f); }),
            
        MenuItem::Value("Rate (Hz)",
            [&state]() { return (int)(state.chorus.getLfoFreq() * 10); },
            [&state](int v) { state.chorus.setLfoFreq(((float)v + 0.5f) / 10.0f); },
            1, 50, 1), // 0.1-5.0Hz
            
        MenuItem::Value("Depth",
            [&state]() { return (int)(state.chorus.getDepth() * 1000); }, // 0.001-0.020
            [&state](int v) { state.chorus.setDepth(((float)v + 0.5f) / 1000.0f); },
            1, 20, 1),
            
        MenuItem::Value("Delay (ms)",
            [&state]() { return (int)(state.chorus.getBaseDelay() * 1000); },
            [&state](int v) { state.chorus.setBaseDelay(((float)v + 0.5f) / 1000.0f); },
            1, 50, 1) // 1-50ms
    };
}
#endif

#ifdef ENABLE_DELAY
// Delay Menu (matches FxDelay)
std::vector<MenuItem> createDelayMenu(SynthState& state) {
    return {
        MenuItem::Toggle("Enabled",
            [&state]() { return state.delayfx.getFeedback() > 0.0f; },
            [&state](int v) { state.delayfx.setFeedback(v ? 0.2f : 0.0f); }),
            
        MenuItem::Value("Feedback",
            [&state]() { return (int)(state.delayfx.getFeedback() * 100); },
            [&state](int v) { state.delayfx.setFeedback((float)(v + 0.5f) / 100.0f); }),
            0, 100,1),
            
        MenuItem::Value("Time (ms)",
            [&state]() { return (int)(state.delayfx.getDelayTime() * 1000); },
            [&state](int v) { state.delayfx.setCustomLength((float)(v + 0.5f) / 1000.0f); },
            1, 1000, 10), // 1-1000ms
            
        MenuItem::Option("Mode",
            {"Normal", "PingPong"},
            [&state]() { return (int)state.delayfx.getMode(); },
            [&state](int v) { state.delayfx.setMode(static_cast<DelayMode>(v)); })
    };
}
#endif

// Channel Menu
std::vector<MenuItem> createChannelMenu(Synth& synth, uint8_t channelIdx) {
    return {
        MenuStructure::createProgramMenu(synth, channelIdx),

        MenuItem::Value("Volume",
            [&synth, channelIdx]() { return synth.channels[channelIdx].volume * 100.0f; },
            [&synth, channelIdx](int v) { synth.channels[channelIdx].volume = (float)(v + 0.5f) / 100.0f; }, 
            0, 100, 5),
            
        MenuItem::Value("Pan",
            [&synth, channelIdx]() { return synth.channels[channelIdx].pan * 100.0f; },
            [&synth, channelIdx](int v) { synth.channels[channelIdx].pan = (float)(v + 0.5f) / 100.0f; },
            0, 100, 1),

        MenuItem::Value("Chorus Send",
            [&synth, channelIdx]() { return synth.channels[channelIdx].chorusSend * 100.0f; },
            [&synth, channelIdx](int v) { synth.channels[channelIdx].chorusSend = (float)(v + 0.5f) / 100.0f; },
            0, 100, 1),

        MenuItem::Value("Reverb Send",
            [&synth, channelIdx]() { return synth.channels[channelIdx].reverbSend * 100.0f; },
            [&synth, channelIdx](int v) { synth.channels[channelIdx].reverbSend = (float)(v + 0.5f) / 100.0f; },
            0, 100, 1),

        MenuItem::Value("Delay Send",
            [&synth, channelIdx]() { return synth.channels[channelIdx].delaySend * 100.0f; },
            [&synth, channelIdx](int v) { synth.channels[channelIdx].delaySend = (float)(v + 0.5f) / 100.0f; },
            0, 100, 1),

          
        MenuItem::Value("Cutoff (Hz)",
            [&synth, channelIdx]() { return synth.channels[channelIdx].filterCutoff ; },
            [&synth, channelIdx](int v) { synth.channels[channelIdx].filterCutoff =  (int)(v + 0.5f); synth.channels[channelIdx].recalcFilter(); },
            CH_FILTER_MIN_FREQ, CH_FILTER_MAX_FREQ, 50),
        
        MenuItem::Value("Resonance",
            [&synth, channelIdx]() { return synth.channels[channelIdx].filterResonance * 100.0f / FILTER_MAX_Q; },
            [&synth, channelIdx](int v) { synth.channels[channelIdx].filterResonance = FILTER_MAX_Q * (float)(v + 0.5f) / 100.0f; synth.channels[channelIdx].recalcFilter(); },
            0, 100, 1),
            
        MenuItem::Action("Reset",
            [&synth, channelIdx](TextGUI&) { synth.channels[channelIdx].reset(); })
    };
}

// Root Menu
std::vector<MenuItem> createRootMenu(Synth& synth, SynthState& state) {
    std::vector<MenuItem> menu;
    
    menu.push_back(MenuStructure::createLoadBankMenu(synth));

    // Channels submenu
    menu.push_back(MenuItem::Submenu("Channels", [&synth]() {
        std::vector<MenuItem> items;
        for (uint8_t i = 0; i < 16; i++) {
            items.push_back(MenuItem::Submenu(
                String("Ch ") + (i+1), 
                [&synth, i]() { return createChannelMenu(synth, i); }
            ));
        }
        return items;
    }));

    // Effects
    #ifdef ENABLE_REVERB
    menu.push_back(MenuItem::Submenu("Reverb", 
        [&state]() { return createReverbMenu(state); }));
    #endif
    
    #ifdef ENABLE_DELAY
    menu.push_back(MenuItem::Submenu("Delay", 
        [&state]() { return createDelayMenu(state); }));
    #endif
    
    #ifdef ENABLE_CHORUS
    menu.push_back(MenuItem::Submenu("Chorus", 
        [&state]() { return createChorusMenu(state); }));
    #endif

    // System
    menu.push_back(MenuItem::Submenu("System", [&synth]() {
        return std::vector<MenuItem>{
            MenuItem::Action("Save Settings", [&synth](TextGUI& gui) {
                gui.busyMessage( "Saving setup...");
                delay(300);
                synth.saveSynthState();
            }),
            MenuItem::Action("Load Settings", [&synth](TextGUI& gui) {
                gui.busyMessage( "Loading setup...");
                delay(300);
                synth.loadSynthState();
            }),
            MenuItem::Action("Reset All", [&synth](TextGUI&) {
                synth.GMReset();
            })
        };
    }));

    return menu;
}

} // namespace MenuStructure

#endif