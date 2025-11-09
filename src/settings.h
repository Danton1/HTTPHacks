#ifndef SETTINGS_H
#define SETTINGS_H

#include <string>

class Settings {
public:
    static std::string save_path;
    static std::string voice_notes_path;
    static std::string audio_input_device;
    static bool always_on_top;
    static bool hide_in_taskbar;
    static std::string post_formatter;
    static std::string keybinding_start_stop_recording;
    static std::string keybinding_open_notes_window;

    Settings();
    void reset_settings();
};

class SettingsManager {
public:
    explicit SettingsManager(const std::string& path);
    Settings getSettings() const;
    bool applySettings();
    const std::string& getSettingsPath() const;
    bool writeSettings(const Settings& s);

private:
    std::string settingsPath;
    Settings settings;

    bool readSettings(std::string path);
};

#endif // SETTINGS_H