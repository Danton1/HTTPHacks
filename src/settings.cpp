#include "settings.h"
#include <string>
#include <iostream>
#include <fstream>
#include "settings.h"
#include <iostream>
#include <fstream>

// Settings class implementation
// Define static members
std::string Settings::save_path;
std::string Settings::voice_notes_path;
std::string Settings::audio_input_device;
bool Settings::always_on_top;
bool Settings::hide_in_taskbar;
std::string Settings::post_formatter;
std::string Settings::keybinding_start_stop_recording;
std::string Settings::keybinding_open_notes_window;

// Constructor implementation
Settings::Settings() {
    save_path = "notes.json";
    voice_notes_path = "voice_notes/";
    audio_input_device = "default";
    always_on_top = true;
    hide_in_taskbar = false;
    post_formatter = "{text}";
    keybinding_start_stop_recording = "Ctrl+R";
    keybinding_open_notes_window = "Ctrl+N";
}

// reset_settings implementation
void Settings::reset_settings() {
    save_path = "notes.json";
    voice_notes_path = "voice_notes/";
    audio_input_device = "default";
    always_on_top = true;
    hide_in_taskbar = false;
    post_formatter = "{text}";
    keybinding_start_stop_recording = "Ctrl+R";
    keybinding_open_notes_window = "Ctrl+N";
}


// SettingsManager implementation
SettingsManager::SettingsManager(const std::string& path) : settingsPath(path) {
    settings = Settings();
    if(!readSettings(settingsPath)){
        std::cerr << "Failed to read settings. Using default values." << std::endl;
    }
}

Settings SettingsManager::getSettings() const {
    return settings;
}

bool SettingsManager::applySettings(){
    return readSettings(settingsPath);
}

bool SettingsManager::readSettings(std::string path){
    if(path.empty()){
        std::cerr << "Settings path is empty." << std::endl;
        return false;
    }
    std::string requiredKeys[] = {"save_path", "voice_notes_path", "audio_input_device", "always_on_top", "hide_in_taskbar",
                                  "post_formatter", "keybinding_start_stop_recording", "keybinding_open_notes_window"};

    // Open and parse the settings txt file
    std::ifstream settingsFile;
    settingsFile.open(path);
    if (!settingsFile.is_open()) {
        std::cerr << "Failed to open settings file: " << path << std::endl;
        return false;
    }
    std::string line;
    while (std::getline(settingsFile, line)) {
        for (const auto& key : requiredKeys) {
            if (line.find(key + "=") == 0) {
                std::string value = line.substr(key.length() + 1);
                if (key == "save_path") {
                    Settings::save_path = value;
                } else if (key == "voice_notes_path") {
                    Settings::voice_notes_path = value;
                } else if (key == "audio_input_device") {
                    Settings::audio_input_device = value;
                } else if (key == "always_on_top") {
                    Settings::always_on_top = (value == "true");
                } else if (key == "hide_in_taskbar") {
                    Settings::hide_in_taskbar = (value == "true");
                } else if (key == "post_formatter") {
                    Settings::post_formatter = value;
                } else if (key == "keybinding_start_stop_recording") {
                    Settings::keybinding_start_stop_recording = value;
                } else if (key == "keybinding_open_notes_window") {
                    Settings::keybinding_open_notes_window = value;
                }
            }
        }
    }
    settingsFile.close();
    return true;
}