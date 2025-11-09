/*
    Sticky Notes (SFML 3) — hub-style window:
      - Borderless, always-on-top (Windows), draggable by header
      - Starts on right edge of primary display (Windows)
      - Left: scrollable list of notes; Right: editable note with typing + scroll
      - Controls:
          ESC                -> close
          Ctrl+N             -> new note
          Ctrl+S             -> save notes.json
          Mouse wheel        -> scroll list / note area
          Click list item    -> select note
      - Notes autosave every few seconds and on Ctrl+S
*/

#include "audio_stream.h"
#include <SFML/Graphics.hpp>
#include <SFML/Window.hpp>
#include <optional>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <chrono>
#include <cmath> // std::floor
#include "settings.h"
#include <iostream>
#include <SFML/Audio/SoundRecorder.hpp>
#include <filesystem>

#if defined(_WIN32)
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #include <windows.h>
  #include <SFML/Config.hpp>
#endif

// ---------- Config ----------
static constexpr unsigned HUB_W = 380;
static constexpr unsigned HUB_H = 520;
static const char* FONT_PATH = "C:/Windows/Fonts/segoeui.ttf"; // <-- change if needed
// static const char* SAVE_PATH = "notes.json";

// helper: SFML 3 FloatRect = {position, size}
inline sf::FloatRect makeRect(float x, float y, float w, float h) {
    return sf::FloatRect({x, y}, {w, h});
}

// helper: set a view from a FloatRect (SFML 3 dropped reset())
inline void setViewFromRect(sf::View& v, const sf::FloatRect& r) {
    sf::Vector2f center = { r.position.x + r.size.x * 0.5f,
                            r.position.y + r.size.y * 0.5f };
    v.setCenter(center);
    v.setSize(r.size);
}

// ---------- Win helpers ----------
#if defined(_WIN32)
static void setAlwaysOnTop(sf::Window& window, bool topmost)
{
    HWND hwnd = static_cast<HWND>(window.getNativeHandle());
    if (!hwnd) return;
    SetWindowPos(hwnd,
                 topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static sf::Vector2i rightEdgeStart(unsigned w, unsigned h, int marginX = 16, int marginY = 64)
{
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int x = std::max(0, screenW - static_cast<int>(w) - marginX);
    int y = std::max(0, marginY);
    if (h > static_cast<unsigned>(screenH)) h = static_cast<unsigned>(screenH);
    return {x, y};
}
#else
static void setAlwaysOnTop(sf::Window&, bool) {}
static sf::Vector2i rightEdgeStart(unsigned, unsigned, int = 16, int = 64) { return {50, 50}; }
#endif

// ---------- Data ----------
struct Note {
    std::string base;      // e.g., "note_2025-11-09_18-12-30"
    std::string txtPath;   // full path to .txt
    std::string wavPath;   // full path to .wav (may not exist)
    std::string text;      // full text
    std::string created;   // derived from filename or file time
};

static std::string nowShort()
{
    using namespace std::chrono;
    auto t = system_clock::to_time_t(system_clock::now());
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%H:%M");
    return oss.str();
}

// Build a normalized folder path with trailing slash
static std::string normalizedVoiceDir() {
    std::string dir = Settings::voice_notes_path;
    if (dir.empty()) dir = "voice_notes/"; // fallback
    if (dir.back() != '/' && dir.back() != '\\') dir.push_back('/');
    return dir;
}

// Read a whole file into string (UTF-8 ok)
static std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::string((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
}

static bool spit(const std::string& path, const std::string& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data.data(), (std::streamsize)data.size());
    return true;
}

// Scan Settings::voice_notes_path for note_*.txt/ .wav and produce list
static std::vector<Note> scanVoiceNotes() {
    std::vector<Note> out;
    std::string dir = normalizedVoiceDir();

    std::unordered_map<std::string, Note> map; // base -> Note

    for (auto& p : std::filesystem::directory_iterator(dir)) {
        if (!p.is_regular_file()) continue;
        auto path = p.path().string();
        auto ext  = p.path().extension().string();
        auto stem = p.path().stem().string();   // base name without extension

        if (ext == ".txt" || ext == ".TXT") {
            Note& n = map[stem];
            n.base    = stem;
            n.txtPath = path;
        } else if (ext == ".wav" || ext == ".WAV") {
            Note& n = map[stem];
            n.base    = stem;
            n.wavPath = path;
        }
    }

    // load text + created
    for (auto& kv : map) {
        Note n = kv.second;
        if (!n.txtPath.empty()) {
            n.text = slurp(n.txtPath);
        }
        // derive "created" from filename if matches note_YYYY-mm-dd_HH-MM-SS
        // else use file write time or blank
        n.created = nowShort();
        try {
            auto ftime = std::filesystem::last_write_time(n.txtPath.empty() ? (n.wavPath.empty() ? "" : n.wavPath) : n.txtPath);
            if (!n.txtPath.empty() || !n.wavPath.empty()) {
                auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    ftime - std::filesystem::file_time_type::clock::now()
                    + std::chrono::system_clock::now()
                );
                std::time_t tt = std::chrono::system_clock::to_time_t(sctp);
                std::tm tm{};
            #if defined(_WIN32)
                localtime_s(&tm, &tt);
            #else
                localtime_r(&tt, &tm);
            #endif
                char buf[6];
                std::strftime(buf, sizeof(buf), "%H:%M", &tm);
                n.created = buf;
            }
        } catch (...) {}

        out.push_back(std::move(n));
    }

    // sort newest-first by base name (timestamp in name) or by path time
    std::sort(out.begin(), out.end(), [](const Note& a, const Note& b){
        return a.base > b.base;
    });

    return out;
}

// Save current editor text back to its file
static bool saveNoteText(const Note& n) {
    if (n.txtPath.empty()) return false;
    return spit(n.txtPath, n.text);
}

static std::string makeTimestampBase() {
    using namespace std::chrono;
    auto t = system_clock::to_time_t(system_clock::now());
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "note_%Y-%m-%d_%H-%M-%S", &tm);
    return std::string(buf);
}

// Create a text-only note file on disk and return the Note
static Note createNewTextNote() {
    std::string dir  = normalizedVoiceDir();
    std::filesystem::create_directories(dir);

    std::string base = makeTimestampBase();
    std::string txt  = dir + base + ".txt";
    std::string wav  = dir + base + ".wav"; // may or may not exist later

    std::string initial = "New note\n";
    spit(txt, initial);

    Note n;
    n.base    = base;
    n.txtPath = txt;
    n.wavPath = wav;
    n.text    = initial;
    n.created = nowShort();
    return n;
}


// utility: split first line
static std::string firstLine(const std::string& s)
{
    auto p = s.find('\n');
    return (p == std::string::npos) ? s : s.substr(0, p);
}

// clamp
template <typename T>
static T clamp(T v, T lo, T hi) { return std::max(lo, std::min(v, hi)); }

// --- modal Settings UI with mic dropdown, blinking caret, spacing, validation ---
static bool openSettingsWindow(sf::Window& parent, SettingsManager& mgr) {
    // Theme (match main window)
    const sf::Color bg(27,27,27);
    const sf::Color panel(38,38,38);
    const sf::Color accent(0,120,215);
    const sf::Color textCol(230,230,230);

    // Window (w,h)
    sf::RenderWindow win(sf::VideoMode({560u, 500u}), "Settings",
                         sf::Style::Titlebar | sf::Style::Close);

    // --- Center on screen (Windows) / else center relative to parent ---
#if defined(_WIN32)
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    auto sSz  = win.getSize();
    win.setPosition({ (screenW - (int)sSz.x) / 2, (screenH - (int)sSz.y) / 2 });
#else
    auto pPos = parent.getPosition();
    auto pSz  = parent.getSize();
    auto sSz2  = win.getSize();
    win.setPosition({ pPos.x + (int)pSz.x/2 - (int)sSz2.x/2,
                      pPos.y + (int)pSz.y/2 - (int)sSz2.y/2 });
#endif

    // Make the dialog front-most while open
    win.setFramerateLimit(120);
    setAlwaysOnTop(win, true);
    win.requestFocus();

    // Font
    sf::Font font;
    bool fontOk = font.openFromFile(FONT_PATH);

    // ------- Load existing values into edit buffers -------
    std::string v_save_path  = Settings::save_path;
    std::string v_voice_dir  = Settings::voice_notes_path;
    std::string v_formatter  = Settings::post_formatter;
    std::string v_hot_rec    = Settings::keybinding_start_stop_recording;
    std::string v_hot_notes  = Settings::keybinding_open_notes_window;
    bool        v_topmost    = Settings::always_on_top;
    bool        v_hide_tb    = Settings::hide_in_taskbar;

    // ------- Mic devices (dropdown) -------
    std::vector<std::string> devices;
    if (sf::SoundRecorder::isAvailable())
        devices = sf::SoundRecorder::getAvailableDevices();
    if (devices.empty()) devices.push_back("No capture devices found");
    std::string defaultDev = sf::SoundRecorder::getDefaultDevice();

    int selectedDev = 0;
    if (!Settings::audio_input_device.empty()) {
        for (int i=0;i<(int)devices.size();++i)
            if (devices[i] == Settings::audio_input_device) { selectedDev = i; break; }
    } else {
        for (int i=0;i<(int)devices.size();++i)
            if (devices[i] == defaultDev) { selectedDev = i; break; }
    }
    std::string v_input_dev = devices[selectedDev];
    bool dropdownOpen = false;

    // ------- Text fields layout (added spacing) -------
    struct Field { std::string* s; sf::FloatRect box; const char* label; };
    std::vector<Field> fields;

    auto makeBox = [&](float y) { return sf::FloatRect({24.f, y}, {512.f, 34.f}); };

    float y = 24.f;
    const float gap = 48.f; // extra spacing between lines

    fields.push_back({ &v_save_path, makeBox(y), "save_path" }); y += gap;
    fields.push_back({ &v_voice_dir, makeBox(y), "voice_notes_path" }); y += gap;
    // microphone dropdown label/box will sit here:
    sf::FloatRect dd_label({24.f, y - 16.f}, {512.f, 14.f});
    sf::FloatRect dd_box  ({24.f, y},        {512.f, 34.f});
    const float rowH = 28.f;
    int dropRows = std::min<int>((int)devices.size(), 8);
    sf::FloatRect dd_drop({dd_box.position.x, dd_box.position.y + dd_box.size.y + 2.f},
                          {dd_box.size.x, dropRows * rowH});
    y += gap;

    fields.push_back({ &v_formatter, makeBox(y), "post_formatter" }); y += gap;
    fields.push_back({ &v_hot_rec,   makeBox(y), "keybinding_start_stop_recording" }); y += gap;
    fields.push_back({ &v_hot_notes, makeBox(y), "keybinding_open_notes_window" }); y += gap;

    // toggles
    sf::FloatRect box_topmost({24.f, y},       {248.f, 28.f});
    sf::FloatRect box_hideTB({288.f, y},       {248.f, 28.f});
    y += 54.f;

    // buttons
    sf::FloatRect btn_ok    ({24.f,  y}, {200.f, 42.f});
    sf::FloatRect btn_cancel({336.f, y}, {200.f, 42.f});

    // focus & caret for text fields
    int focused = -1;
    bool caretOn = true;
    auto lastBlink = std::chrono::steady_clock::now();

    auto drawBox = [&](const sf::FloatRect& r, bool active) {
        sf::RectangleShape rect({r.size.x, r.size.y});
        rect.setPosition(r.position);
        rect.setFillColor(active ? sf::Color(50,50,50) : panel);
        rect.setOutlineColor(active ? accent : sf::Color(70,70,70));
        rect.setOutlineThickness(1.f);
        win.draw(rect);
    };

    auto labelText = [&](const std::string& s, float x, float y, unsigned sz=14) {
        if (!fontOk) return;
        sf::Text t(font, s, sz);
        t.setFillColor(textCol);
        t.setPosition({x, y});
        win.draw(t);
    };

    auto drawBtn = [&](sf::FloatRect r, const char* s){
        sf::RectangleShape b({r.size.x, r.size.y});
        b.setPosition(r.position);
        b.setFillColor(sf::Color(55,55,55));
        b.setOutlineColor(sf::Color(80,80,80));
        b.setOutlineThickness(1.f);
        win.draw(b);
        labelText(s, r.position.x + 16.f, r.position.y + 10.f, 18);
    };

    auto validInputs = [&]()->bool {
        // trivial validation: non-empty fields
        if (v_save_path.empty() || v_voice_dir.empty() || v_formatter.empty())
            return false;
        return true;
    };

    while (win.isOpen()) {
        // caret blink
        auto now = std::chrono::steady_clock::now();
        if (now - lastBlink > std::chrono::milliseconds(500)) {
            caretOn = !caretOn;
            lastBlink = now;
        }

        while (const std::optional ev = win.pollEvent()) {
            if (ev->is<sf::Event::Closed>()) { win.close(); return false; }

            if (ev->is<sf::Event::MouseButtonPressed>()) {
                auto mb = ev->getIf<sf::Event::MouseButtonPressed>();
                if (mb->button == sf::Mouse::Button::Left) {
                    sf::Vector2f mp = sf::Vector2f(sf::Mouse::getPosition(win));

                    // Dropdown interactions first
                    if (dd_box.contains(mp)) {
                        dropdownOpen = !dropdownOpen;
                        focused = -1;
                    } else if (dropdownOpen && dd_drop.contains(mp)) {
                        int idx = (int)((mp.y - dd_drop.position.y) / rowH);
                        idx = clamp(idx, 0, (int)devices.size() - 1);
                        selectedDev = idx;
                        v_input_dev = devices[selectedDev];
                        dropdownOpen = false;
                    } else {
                        // Close dropdown if click elsewhere
                        dropdownOpen = false;

                        // Focus text fields
                        focused = -1;
                        for (int i = 0; i < (int)fields.size(); ++i) {
                            if (fields[i].box.contains(mp)) { focused = i; break; }
                        }

                        // Toggles
                        if (box_topmost.contains(mp)) v_topmost = !v_topmost;
                        if (box_hideTB.contains(mp)) v_hide_tb = !v_hide_tb;

                        // Buttons
                        if (btn_ok.contains(mp)) {
                            // Validate -> write -> apply
                            if (!validInputs()) {
                                std::cerr << "Invalid inputs; please fill all required fields.\n";
                            } else {
                                Settings::save_path = v_save_path;
                                Settings::voice_notes_path = v_voice_dir;
                                Settings::audio_input_device = v_input_dev;
                                Settings::post_formatter = v_formatter;
                                Settings::keybinding_start_stop_recording = v_hot_rec;
                                Settings::keybinding_open_notes_window   = v_hot_notes;
                                Settings::always_on_top   = v_topmost;
                                Settings::hide_in_taskbar = v_hide_tb;

                                if (!mgr.writeSettings(Settings())) {
                                    std::cerr << "Failed writing settings file.\n";
                                } else {
                                    mgr.applySettings(); // re-read settings.txt
                                }
                                win.close();
                                return true;
                            }
                        }
                        if (btn_cancel.contains(mp)) { win.close(); return false; }
                    }
                }
            }

            if (ev->is<sf::Event::KeyPressed>()) {
                auto k = ev->getIf<sf::Event::KeyPressed>();
                if (k->scancode == sf::Keyboard::Scancode::Escape) { win.close(); return false; }

                if (dropdownOpen) {
                    if (k->scancode == sf::Keyboard::Scancode::Up) {
                        selectedDev = std::max(0, selectedDev - 1);
                        v_input_dev = devices[selectedDev];
                    } else if (k->scancode == sf::Keyboard::Scancode::Down) {
                        selectedDev = std::min((int)devices.size() - 1, selectedDev + 1);
                        v_input_dev = devices[selectedDev];
                    } else if (k->scancode == sf::Keyboard::Scancode::Enter) {
                        dropdownOpen = false;
                    }
                    continue;
                }

                if (k->scancode == sf::Keyboard::Scancode::Tab) {
                    if (!fields.empty()) focused = (focused + 1) % (int)fields.size();
                } else if (k->scancode == sf::Keyboard::Scancode::Backspace && focused >= 0) {
                    auto& s = *fields[focused].s;
                    if (!s.empty()) s.pop_back();
                } else if (k->scancode == sf::Keyboard::Scancode::Enter) {
                    // Treat Enter like OK
                    if (validInputs()) {
                        Settings::save_path = v_save_path;
                        Settings::voice_notes_path = v_voice_dir;
                        Settings::audio_input_device = v_input_dev;
                        Settings::post_formatter = v_formatter;
                        Settings::keybinding_start_stop_recording = v_hot_rec;
                        Settings::keybinding_open_notes_window   = v_hot_notes;
                        Settings::always_on_top   = v_topmost;
                        Settings::hide_in_taskbar = v_hide_tb;

                        if (!mgr.writeSettings(Settings())) {
                            std::cerr << "Failed writing settings file.\n";
                        } else {
                            mgr.applySettings();
                        }
                        win.close();
                        return true;
                    } else {
                        std::cerr << "Invalid inputs; please fill all required fields.\n";
                    }
                }
            }

            if (ev->is<sf::Event::TextEntered>() && focused >= 0 && !dropdownOpen) {
                auto t = ev->getIf<sf::Event::TextEntered>();
                uint32_t uc = t->unicode;
                if (uc >= 32 && uc != 127) {
                    (*fields[focused].s) += static_cast<char>(uc);
                }
            }
        }

        // ----- draw -----
        win.clear(bg);

        // Draw text inputs
        for (int i = 0; i < (int)fields.size(); ++i) {
            const bool isActive = (i == focused);
            drawBox(fields[i].box, isActive);
            labelText(fields[i].label, fields[i].box.position.x + 6.f, fields[i].box.position.y - 18.f);
            if (fontOk) {
                sf::Text val(font, *fields[i].s, 16);
                val.setFillColor(textCol);
                val.setPosition({fields[i].box.position.x + 8.f, fields[i].box.position.y + 6.f});
                win.draw(val);

                // Blinking caret for active text box
                if (isActive && caretOn) {
                    auto b = val.getLocalBounds();
                    sf::RectangleShape caret({1.5f, 18.f});
                    caret.setFillColor(accent);
                    caret.setPosition({val.getPosition().x + b.size.x + 2.f, val.getPosition().y + 2.f});
                    win.draw(caret);
                }
            }
        }

        // Mic dropdown (collapsed face)
        labelText("audio_input_device", dd_label.position.x, dd_label.position.y);
        drawBox(dd_box, false);
        if (fontOk) {
            // Ellipsize if needed
            auto fits = [&](const std::string& s){
                sf::Text tmp(font, s, 16);
                return tmp.getLocalBounds().size.x <= dd_box.size.x - 16.f;
            };
            std::string shown = v_input_dev;
            if (!fits(shown)) {
                while (!shown.empty() && !fits(shown + "...")) shown.pop_back();
                shown += "...";
            }
            sf::Text cur(font, shown, 16);
            cur.setFillColor(textCol);
            cur.setPosition({dd_box.position.x + 8.f, dd_box.position.y + 6.f});
            win.draw(cur);
        }
        // little caret indicator
        sf::RectangleShape tri({10.f, 2.f});
        tri.setFillColor(textCol);
        tri.setPosition({dd_box.position.x + dd_box.size.x - 16.f, dd_box.position.y + dd_box.size.y/2.f - 1.f});
        win.draw(tri);

        // Toggles
        drawBox(box_topmost, false);
        labelText(std::string("always_on_top: ") + (v_topmost ? "true" : "false"),
                  box_topmost.position.x + 8.f, box_topmost.position.y + 4.f, 16);
        drawBox(box_hideTB, false);
        labelText(std::string("hide_in_taskbar: ") + (v_hide_tb ? "true" : "false"),
                  box_hideTB.position.x + 8.f, box_hideTB.position.y + 4.f, 16);

        // Buttons
        drawBtn(btn_ok, "OK");
        drawBtn(btn_cancel, "Cancel");

        // --- Draw dropdown LAST so it overlays everything ---
        if (dropdownOpen) {
            // dim background a bit
            sf::RectangleShape dim({(float)win.getSize().x, (float)win.getSize().y});
            dim.setPosition({0.f, 0.f});
            dim.setFillColor(sf::Color(0, 0, 0, 40));
            win.draw(dim);

            sf::RectangleShape drop({dd_drop.size.x, dd_drop.size.y});
            drop.setPosition(dd_drop.position);
            drop.setFillColor(panel);
            drop.setOutlineThickness(1.f);
            drop.setOutlineColor(sf::Color(70,70,70));
            win.draw(drop);

            for (int i = 0; i < dropRows; ++i) {
                int idx = i;
                sf::FloatRect row({dd_drop.position.x, dd_drop.position.y + i * rowH},
                                  {dd_drop.size.x, rowH});
                if (idx == selectedDev) {
                    sf::RectangleShape hi({row.size.x, row.size.y});
                    hi.setPosition(row.position);
                    hi.setFillColor(sf::Color(55,55,55));
                    win.draw(hi);
                }
                if (fontOk) {
                    sf::Text t(font, devices[idx], 15);
                    t.setFillColor(textCol);
                    t.setPosition({row.position.x + 8.f, row.position.y + 5.f});
                    win.draw(t);
                }
            }
        }

        win.display();
    }
    return false;
}


// ---------- App ----------
int main()
{
    // Window
    sf::RenderWindow win(sf::VideoMode({HUB_W, HUB_H}), "Voice Notes", sf::Style::None);
    // Settings
    SettingsManager settingsMgr("settings.txt");
    settingsMgr.applySettings();                     // load on start (reads file or creates defaults)
    setAlwaysOnTop(win, Settings::always_on_top);    // honor setting immediately

    win.setFramerateLimit(144);
    setAlwaysOnTop(win, true);
    win.setPosition(rightEdgeStart(HUB_W, HUB_H));

    // Dragging by header
    bool dragging = false;
    sf::Vector2i dragOffset{0,0};

    // Colors
    const sf::Color bg(27,27,27);
    const sf::Color panel(38,38,38);
    const sf::Color header(45,45,45);
    const sf::Color accent(0,120,215);
    const sf::Color textCol(230,230,230);
    const sf::Color muted(170,170,170);
    const sf::Color sel(60,60,60);

    // Layout
    const float headerH = 36.f;
    const float listW   = 180.f;
    const float itemH   = 56.f;

    // Geometry
    sf::RectangleShape headerRect({static_cast<float>(HUB_W), headerH});
    headerRect.setPosition(sf::Vector2f(0.f, 0.f));
    headerRect.setFillColor(header);

    sf::RectangleShape listRect({listW, static_cast<float>(HUB_H) - headerH});
    listRect.setPosition(sf::Vector2f(0.f, headerH));
    listRect.setFillColor(panel);

    sf::RectangleShape editorRect({static_cast<float>(HUB_W) - listW, static_cast<float>(HUB_H) - headerH});
    editorRect.setPosition(sf::Vector2f(listW, headerH));
    editorRect.setFillColor(bg);

    // Font
    sf::Font font;
    bool ok = font.openFromFile(FONT_PATH); (void)ok; // avoid nodiscard warning

    // Text UI
    sf::Text titleText(font, "Voice Notes", 16);
    titleText.setFillColor(textCol);
    titleText.setPosition(sf::Vector2f(10.f, 8.f));

    // Buttons (+) and (×)
    sf::Text plus(font, "+", 20);
    plus.setFillColor(textCol);
    plus.setPosition(sf::Vector2f(static_cast<float>(HUB_W) - 58.f, 6.f));
    sf::FloatRect plusBounds = plus.getGlobalBounds();

    sf::Text closeX(font, "x", 18);
    closeX.setFillColor(textCol);
    closeX.setPosition(sf::Vector2f(static_cast<float>(HUB_W) - 26.f, 6.f));
    sf::FloatRect closeBounds = closeX.getGlobalBounds();

    // Record button
    sf::Text microphone(font, "mic", 20);
    microphone.setFillColor(textCol);
    microphone.setPosition(sf::Vector2f(static_cast<float>(HUB_W) - 110.f, 6.f));
    sf::FloatRect microphoneBounds = microphone.getGlobalBounds();

    // Settings gear (⚙️)
    sf::Text gear(font, u8"\u2699", 20);
    // Play/Pause button for selected note
    sf::Text playBtn(font, "▶", 18);
    playBtn.setFillColor(textCol);
    playBtn.setPosition(sf::Vector2f(static_cast<float>(HUB_W) - 140.f, 7.f));
    sf::FloatRect playBounds = playBtn.getGlobalBounds();

    // Simple audio player state
    sf::SoundBuffer playBuffer;
    sf::Sound       player;
    bool            isPlaying = false;

    auto playSelected = [&](){
        if (selected < 0 || selected >= (int)notes.size()) return;
        const auto& n = notes[selected];
        if (n.wavPath.empty()) return;
        if (!std::filesystem::exists(n.wavPath)) return;

        if (isPlaying) {
            player.stop();
            isPlaying = false;
            playBtn.setString("▶");
            return;
        }
        if (!playBuffer.loadFromFile(n.wavPath)) {
            std::cerr << "Failed to load " << n.wavPath << "\n";
            return;
        }
        player.setBuffer(playBuffer);
        player.play();
        isPlaying = true;
        playBtn.setString("⏸");
    };

    gear.setFillColor(textCol);
    gear.setPosition(sf::Vector2f(static_cast<float>(HUB_W) - 92.f, 6.f)); // between mic and plus
    sf::FloatRect gearBounds = gear.getGlobalBounds();


    // Notes
    std::vector<Note> notes = scanVoiceNotes();
    if (notes.empty()) {
        notes.push_back({std::string("Take a note...\n"), nowShort()});
    }
    int selected = 0;

    // Scrolling
    float listScroll = 0.f;
    float editorScroll = 0.f;

    // Text rendering objects
    sf::Text listLine(font, "", 14);
    listLine.setFillColor(textCol);

    sf::Text editorText(font, "", 16);
    editorText.setFillColor(textCol);
    editorText.setLineSpacing(1.2f);

    // Views for clipping/scroll
    sf::View listView(makeRect(0.f, 0.f, listW, static_cast<float>(HUB_H) - headerH));
    listView.setViewport(makeRect(0.f, headerH / HUB_H,
                                  listW / static_cast<float>(HUB_W),
                                  (static_cast<float>(HUB_H) - headerH) / HUB_H));

    sf::View editorView(makeRect(0.f, 0.f,
                                 static_cast<float>(HUB_W) - listW,
                                 static_cast<float>(HUB_H) - headerH));
    editorView.setViewport(makeRect(listW / static_cast<float>(HUB_W), headerH / HUB_H,
                                    (static_cast<float>(HUB_W) - listW) / HUB_W,
                                    (static_cast<float>(HUB_H) - headerH) / HUB_H));

    auto requestSaveAt = std::chrono::steady_clock::now();
    auto maybeAutosave = [&](){
        auto now = std::chrono::steady_clock::now();
        if (now - requestSaveAt > std::chrono::seconds(3)) {
            if (selected >= 0 && selected < (int)notes.size()) {
                saveNoteText(notes[selected]);
            }
            requestSaveAt = now;
        }
    };

    while (win.isOpen())
    {
        while (const std::optional ev = win.pollEvent())
        {
            if (ev->is<sf::Event::Closed>()) win.close();

            if (ev->is<sf::Event::KeyPressed>()) {
                auto k = ev->getIf<sf::Event::KeyPressed>();
                if (k->scancode == sf::Keyboard::Scancode::Escape) {
                    win.close();
                }
                // Ctrl+N: new note
                if (k->control && k->scancode == sf::Keyboard::Scancode::N) {
                    auto n = createNewTextNote();
                    notes.push_back(std::move(n));
                    selected = (int)notes.size() - 1;
                    editorScroll = 0.f;
                    requestSaveAt = std::chrono::steady_clock::now() - std::chrono::seconds(10);
                }
                // Ctrl+S: save selected note to its .txt
                if (k->control && k->scancode == sf::Keyboard::Scancode::S) {
                    if (selected >= 0 && selected < (int)notes.size()) {
                        saveNoteText(notes[selected]);
                    }
                    requestSaveAt = std::chrono::steady_clock::now();
                }

                // Backspace handling (editor only)
                if (k->scancode == sf::Keyboard::Scancode::Backspace) {
                    if (!notes[selected].text.empty()) {
                        notes[selected].text.pop_back();
                        notes[selected].created = nowShort();
                        requestSaveAt = std::chrono::steady_clock::now();
                    }
                }
                // Enter -> newline
                if (k->scancode == sf::Keyboard::Scancode::Enter) {
                    notes[selected].text.push_back('\n');
                    notes[selected].created = nowShort();
                    requestSaveAt = std::chrono::steady_clock::now();
                }
            }

            // Typing unicode (printable)
            if (ev->is<sf::Event::TextEntered>()) {
                auto t = ev->getIf<sf::Event::TextEntered>();
                uint32_t uc = t->unicode;
                if (uc >= 32 && uc != 127) { // skip control chars
                    notes[selected].text += static_cast<char>(uc);
                    notes[selected].created = nowShort();
                    requestSaveAt = std::chrono::steady_clock::now();
                }
            }

            // Mouse wheel -> scroll list/editor depending on cursor
            if (ev->is<sf::Event::MouseWheelScrolled>()) {
                auto w = ev->getIf<sf::Event::MouseWheelScrolled>();
                sf::Vector2f mp = sf::Vector2f(sf::Mouse::getPosition(win));
                if (listRect.getGlobalBounds().contains(mp)) {
                    listScroll -= w->delta * 30.f;
                    listScroll = std::max(0.f, listScroll);
                } else if (editorRect.getGlobalBounds().contains(mp)) {
                    editorScroll -= w->delta * 40.f;
                    editorScroll = std::max(0.f, editorScroll);
                }
            }

            // Mouse down for drag and clicks
            if (ev->is<sf::Event::MouseButtonPressed>()) {
                auto mb = ev->getIf<sf::Event::MouseButtonPressed>();
                if (mb->button == sf::Mouse::Button::Left) {
                    sf::Vector2f mp = sf::Vector2f(sf::Mouse::getPosition(win));

                    // Header: drag or buttons
                    if (headerRect.getGlobalBounds().contains(mp)) {
                        if (closeBounds.contains(mp)) { win.close(); }
                        else if (plusBounds.contains(mp)) {
                            auto n = createNewTextNote();
                            notes.push_back(std::move(n));
                            selected = (int)notes.size() - 1;
                            editorScroll = 0.f;
                            requestSaveAt = std::chrono::steady_clock::now() - std::chrono::seconds(10);
                        } else if (gearBounds.contains(mp)) {
                            // Make sure the settings dialog isn’t hidden
                            setAlwaysOnTop(win, false);
                            if (openSettingsWindow(win, settingsMgr)) {
                                // Re-apply settings that affect the main window
                                setAlwaysOnTop(win, Settings::always_on_top);
                            } else {
                                // Even on cancel, restore to current setting
                                setAlwaysOnTop(win, Settings::always_on_top);
                            }
                        } else if (playBounds.contains(mp)) {
                            playSelected();
                        } else if(microphoneBounds.contains(mp)) {
                            if(microphone.getString() == "mic") {
                                startRecordAudioFromMicrophone();
                                microphone.setString("stop");
                                win.draw(microphone);   
                            } else {
                                stopRecordAudioFromMicrophone();

                                // Refresh list and select newest note (assumes filename timestamp ordering)
                                auto beforeCount = notes.size();
                                notes = scanVoiceNotes();
                                if (!notes.empty()) {
                                    selected = 0; // because we sort newest-first by base
                                    // Optionally, find exact newest by comparing txt/wav times if you prefer
                                }

                                microphone.setString("mic");
                                win.draw(microphone);
                            }
                        } else {
                            dragging = true;
                            sf::Vector2i mouseScreen = sf::Mouse::getPosition();
                            dragOffset = mouseScreen - win.getPosition();
                        }
                    }

                    // List item click -> select
                    if (listRect.getGlobalBounds().contains(mp)) {
                        float y = mp.y - listRect.getPosition().y + listScroll;
                        int idx = static_cast<int>(std::floor(y / itemH));
                        if (idx >= 0 && idx < static_cast<int>(notes.size())) {
                            selected = idx;
                            // refresh text from disk
                            if (!notes[selected].txtPath.empty()) {
                                notes[selected].text = slurp(notes[selected].txtPath);
                            }
                            editorScroll = 0.f;
                            // stop playback if switching notes
                            if (isPlaying) {
                                player.stop();
                                isPlaying = false;
                                playBtn.setString("▶");
                            }
                        }

                    }
                }
            }
            if (ev->is<sf::Event::MouseButtonReleased>()) {
                auto mr = ev->getIf<sf::Event::MouseButtonReleased>();
                if (mr->button == sf::Mouse::Button::Left) dragging = false;
            }
            if (ev->is<sf::Event::MouseMoved>()) {
                if (dragging) {
                    sf::Vector2i mouseScreen = sf::Mouse::getPosition();
                    win.setPosition(mouseScreen - dragOffset);
                }
            }

            // Keep views updated on resize (if you ever add resize)
            if (ev->is<sf::Event::Resized>()) {
                auto sz = win.getSize();
                headerRect.setSize(sf::Vector2f(static_cast<float>(sz.x), headerH));
                listRect.setSize(sf::Vector2f(listW, static_cast<float>(sz.y) - headerH));
                editorRect.setSize(sf::Vector2f(static_cast<float>(sz.x) - listW, static_cast<float>(sz.y) - headerH));

                setViewFromRect(listView,   makeRect(0.f, 0.f, listW, static_cast<float>(sz.y) - headerH));
                listView.setViewport(makeRect(0.f, headerH / sz.y,
                                              listW / static_cast<float>(sz.x),
                                              (static_cast<float>(sz.y) - headerH) / sz.y));

                setViewFromRect(editorView, makeRect(0.f, 0.f,
                                                     static_cast<float>(sz.x) - listW,
                                                     static_cast<float>(sz.y) - headerH));
                editorView.setViewport(makeRect(listW / static_cast<float>(sz.x), headerH / sz.y,
                                                (static_cast<float>(sz.x) - listW) / sz.x,
                                                (static_cast<float>(sz.y) - headerH) / sz.y));
            }
        }

        // Autosave throttle
        maybeAutosave();

        // ---------- Draw ----------
        win.clear(bg);

        // Header
        win.draw(headerRect);
        if (font.getInfo().family.size()) {
            win.draw(titleText);
            win.draw(plus);
            win.draw(closeX);
            win.draw(playBtn);
            win.draw(gear);
            win.draw(microphone);
        }

        // List panel
        win.draw(listRect);
        win.setView(listView);
        {
            float y0 = -listScroll;
            for (size_t i = 0; i < notes.size(); ++i) {
                sf::FloatRect row = makeRect(0.f, y0 + static_cast<float>(i) * itemH, listW, itemH - 1.f);

                sf::RectangleShape rowBg(sf::Vector2f(row.size.x, row.size.y));
                rowBg.setPosition(sf::Vector2f(row.position.x, row.position.y));
                rowBg.setFillColor(static_cast<int>(i) == selected ? sel : panel);
                win.draw(rowBg);

                if (font.getInfo().family.size()) {
                    std::string ttl = firstLine(notes[i].text);
                    if (ttl.empty()) ttl = "(empty)";
                    if (ttl.size() > 20) ttl = ttl.substr(0, 20) + "...";

                    listLine.setString(ttl);
                    listLine.setPosition(sf::Vector2f(8.f, row.position.y + 8.f));
                    listLine.setFillColor(textCol);
                    win.draw(listLine);

                    // timestamp (muted, right-aligned)
                    sf::Text ts(font, notes[i].created, 12);
                    ts.setFillColor(muted);
                    auto bounds = ts.getLocalBounds(); // position/size
                    ts.setPosition(sf::Vector2f(row.position.x + row.size.x - bounds.size.x - 8.f,
                                                row.position.y + 6.f));
                    win.draw(ts);
                }
            }
        }
        win.setView(win.getDefaultView());

        // Editor panel
        win.draw(editorRect);
        win.setView(editorView);
        {
            if (font.getInfo().family.size()) {
                editorText.setString(notes[selected].text);
                editorText.setPosition(sf::Vector2f(8.f, 8.f - editorScroll));
                win.draw(editorText);

                // simple caret (blink)
                static bool on = true;
                static auto last = std::chrono::steady_clock::now();
                auto now = std::chrono::steady_clock::now();
                if (now - last > std::chrono::milliseconds(500)) { on = !on; last = now; }
                if (on) {
                    auto b = editorText.getGlobalBounds(); // position/size
                    sf::RectangleShape caret(sf::Vector2f(1.5f, editorText.getCharacterSize() * 1.25f));
                    caret.setFillColor(accent);
                    caret.setPosition(sf::Vector2f(8.f + b.size.x, 8.f - editorScroll + 2.f));
                    win.draw(caret);
                }
            }
        }
        win.setView(win.getDefaultView());

        // If finished playing, reset icon
        if (isPlaying && player.getStatus() != sf::Sound::Playing) {
            isPlaying = false;
            playBtn.setString("▶");
        }

        win.display();
    }
    return 0;
}
