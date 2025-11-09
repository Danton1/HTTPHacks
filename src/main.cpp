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
static const char* SAVE_PATH = "notes.json";

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
    std::string text;    // first line acts as title; rest is body
    std::string created; // HH:MM or date-like string for list
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

// naive JSON escape for quotes/newlines
static std::string jsonEscape(const std::string& s)
{
    std::ostringstream o;
    for (char c : s) {
        switch (c) {
            case '\"': o << "\\\""; break;
            case '\\': o << "\\\\"; break;
            case '\n': o << "\\n";  break;
            case '\r':             break;
            case '\t': o << "\\t";  break;
            default:   o << c;      break;
        }
    }
    return o.str();
}

static void saveNotes(const std::vector<Note>& notes)
{
    std::ofstream f(SAVE_PATH, std::ios::binary);
    if (!f) return;
    f << "{ \"notes\": [";
    for (size_t i = 0; i < notes.size(); ++i) {
        if (i) f << ",";
        f << "{\"text\":\""   << jsonEscape(notes[i].text)
          << "\",\"created\":\"" << jsonEscape(notes[i].created) << "\"}";
    }
    f << "] }";
}

static std::vector<Note> loadNotes()
{
    std::ifstream f(SAVE_PATH, std::ios::binary);
    if (!f) return {};
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<Note> out;
    // ultra-lightweight parse: look for "text":"...","created":"..."
    size_t pos = 0;
    while (true) {
        auto t1 = s.find("\"text\":\"", pos);
        if (t1 == std::string::npos) break;
        t1 += 8;
        std::string text;
        for (size_t i = t1; i < s.size(); ++i) {
            if (s[i] == '\\') { // escape
                if (i + 1 < s.size()) {
                    char n = s[++i];
                    if (n == 'n') text.push_back('\n');
                    else if (n == 't') text.push_back('\t');
                    else text.push_back(n);
                }
            } else if (s[i] == '\"') {
                pos = i + 1;
                break;
            } else text.push_back(s[i]);
        }
        auto c1 = s.find("\"created\":\"", pos);
        if (c1 == std::string::npos) break;
        c1 += 11;
        std::string created;
        for (size_t i = c1; i < s.size(); ++i) {
            if (s[i] == '\\') {
                if (i + 1 < s.size()) created.push_back(s[++i]);
            } else if (s[i] == '\"') {
                pos = i + 1;
                break;
            } else created.push_back(s[i]);
        }
        out.push_back({text, created});
    }
    return out;
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
    font.openFromFile(FONT_PATH); // SFML 3: openFromFile

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

    // Notes
    std::vector<Note> notes = loadNotes();
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
            saveNotes(notes);
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
                    notes.push_back({std::string("New note\n"), nowShort()});
                    selected = static_cast<int>(notes.size()) - 1;
                    editorScroll = 0.f;
                    requestSaveAt = std::chrono::steady_clock::now() - std::chrono::seconds(10);
                }
                // Ctrl+S: save
                if (k->control && k->scancode == sf::Keyboard::Scancode::S) {
                    saveNotes(notes);
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
                            notes.push_back({std::string("New note\n"), nowShort()});
                            selected = static_cast<int>(notes.size()) - 1;
                            editorScroll = 0.f;
                            requestSaveAt = std::chrono::steady_clock::now() - std::chrono::seconds(10);
                        } else if(microphoneBounds.contains(mp)) {
                            if(microphone.getString() == "mic") {
                                startRecordAudioFromMicrophone();
                                microphone.setString("stop");
                                win.draw(microphone);   
                            } else {
                                stopRecordAudioFromMicrophone();
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
                            editorScroll = 0.f;
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

        win.display();
    }
    return 0;
}
