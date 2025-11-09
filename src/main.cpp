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

// --- modal Settings UI ---
static bool openSettingsWindow(sf::Window& parent, SettingsManager& mgr) {
    // Match main window palette
    const sf::Color bg(27,27,27);
    const sf::Color panel(38,38,38);
    const sf::Color accent(0,120,215);
    const sf::Color textCol(230,230,230);

    // Correct size (W,H)
    sf::RenderWindow win(sf::VideoMode({520u, 420u}), "Settings",
                         sf::Style::Titlebar | sf::Style::Close);

    // Center relative to parent
    auto pPos = parent.getPosition();
    auto pSz  = parent.getSize();
    auto sSz  = win.getSize();
    win.setPosition({ pPos.x + (int)pSz.x/2 - (int)sSz.x/2,
                      pPos.y + (int)pSz.y/2 - (int)sSz.y/2 });

    win.setFramerateLimit(120);

    // Put dialog above and focus it
    setAlwaysOnTop(win, true);
    win.requestFocus();

    // Font — reuse style, check result to avoid [[nodiscard]] warning
    sf::Font font;
    bool fontOk = font.openFromFile(FONT_PATH);

    // Editable copies of current values
    std::string v_save_path  = Settings::save_path;
    std::string v_voice_dir  = Settings::voice_notes_path;
    std::string v_input_dev  = Settings::audio_input_device;
    bool v_topmost           = Settings::always_on_top;
    bool v_hide_taskbar      = Settings::hide_in_taskbar;
    std::string v_formatter  = Settings::post_formatter;
    std::string v_hot_rec    = Settings::keybinding_start_stop_recording;
    std::string v_hot_notes  = Settings::keybinding_open_notes_window;

    // Simple text fields as rectangles with focus index
    struct Field { std::string* s; sf::FloatRect box; const char* label; };
    std::vector<Field> fields;

    auto makeBox = [&](float y) { return sf::FloatRect({20.f, y}, {480.f, 30.f}); };

    fields.push_back({ &v_save_path, makeBox(20.f),  "save_path" });
    fields.push_back({ &v_voice_dir, makeBox(60.f),  "voice_notes_path" });
    fields.push_back({ &v_input_dev, makeBox(100.f), "audio_input_device" });
    fields.push_back({ &v_formatter, makeBox(140.f), "post_formatter" });
    fields.push_back({ &v_hot_rec,   makeBox(180.f), "keybinding_start_stop_recording" });
    fields.push_back({ &v_hot_notes, makeBox(220.f), "keybinding_open_notes_window" });

    sf::FloatRect box_topmost({20.f, 270.f}, {220.f, 26.f});
    sf::FloatRect box_hideTB({260.f, 270.f}, {220.f, 26.f});

    sf::FloatRect btn_ok   ({20.f, 340.f}, {180.f, 40.f});
    sf::FloatRect btn_cancel({320.f, 340.f}, {180.f, 40.f});

    int focused = -1;

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
        labelText(s, r.position.x + 14.f, r.position.y + 10.f, 18);
    };

    while (win.isOpen()) {
        while (const std::optional ev = win.pollEvent()) {
            if (ev->is<sf::Event::Closed>()) { win.close(); return false; }

            if (ev->is<sf::Event::MouseButtonPressed>()) {
                auto mb = ev->getIf<sf::Event::MouseButtonPressed>();
                if (mb->button == sf::Mouse::Button::Left) {
                    sf::Vector2f mp = sf::Vector2f(sf::Mouse::getPosition(win));

                    // focus text fields
                    focused = -1;
                    for (int i = 0; i < (int)fields.size(); ++i) {
                        if (fields[i].box.contains(mp)) { focused = i; break; }
                    }

                    // toggles
                    if (box_topmost.contains(mp)) v_topmost = !v_topmost;
                    if (box_hideTB.contains(mp)) v_hide_taskbar = !v_hide_taskbar;

                    // buttons
                    if (btn_ok.contains(mp)) {
                        // Push values into Settings statics
                        Settings::save_path = v_save_path;
                        Settings::voice_notes_path = v_voice_dir;
                        Settings::audio_input_device = v_input_dev;
                        Settings::post_formatter = v_formatter;
                        Settings::keybinding_start_stop_recording = v_hot_rec;
                        Settings::keybinding_open_notes_window = v_hot_notes;
                        Settings::always_on_top = v_topmost;
                        Settings::hide_in_taskbar = v_hide_taskbar;

                        // Write to disk and re-load
                        if (!mgr.writeSettings(Settings())) {
                            std::cerr << "Failed writing settings file.\n";
                        } else {
                            mgr.applySettings();
                        }
                        win.close();
                        return true;
                    }
                    if (btn_cancel.contains(mp)) { win.close(); return false; }
                }
            }

            if (ev->is<sf::Event::KeyPressed>()) {
                auto k = ev->getIf<sf::Event::KeyPressed>();
                if (k->scancode == sf::Keyboard::Scancode::Escape) { win.close(); return false; }
                if (k->scancode == sf::Keyboard::Scancode::Tab) {
                    if (fields.empty()) continue;
                    focused = (focused + 1) % (int)fields.size();
                }
                if (k->scancode == sf::Keyboard::Scancode::Enter) {
                    // OK via Enter
                    Settings::save_path = v_save_path;
                    Settings::voice_notes_path = v_voice_dir;
                    Settings::audio_input_device = v_input_dev;
                    Settings::post_formatter = v_formatter;
                    Settings::keybinding_start_stop_recording = v_hot_rec;
                    Settings::keybinding_open_notes_window = v_hot_notes;
                    Settings::always_on_top = v_topmost;
                    Settings::hide_in_taskbar = v_hide_taskbar;

                    if (!mgr.writeSettings(Settings())) {
                        std::cerr << "Failed writing settings file.\n";
                    } else {
                        mgr.applySettings();
                    }
                    win.close();
                    return true;
                }
                if (k->scancode == sf::Keyboard::Scancode::Backspace && focused >= 0) {
                    auto& s = *fields[focused].s;
                    if (!s.empty()) s.pop_back();
                }
            }

            if (ev->is<sf::Event::TextEntered>() && focused >= 0) {
                auto t = ev->getIf<sf::Event::TextEntered>();
                uint32_t uc = t->unicode;
                if (uc >= 32 && uc != 127) {
                    (*fields[focused].s) += static_cast<char>(uc);
                }
            }
        }

        // ----- draw -----
        win.clear(bg);

        // text boxes
        for (int i = 0; i < (int)fields.size(); ++i) {
            drawBox(fields[i].box, i == focused);
            labelText(fields[i].label, fields[i].box.position.x + 6.f, fields[i].box.position.y - 16.f);
            if (fontOk) {
                sf::Text val(font, *fields[i].s, 16);
                val.setFillColor(textCol);
                val.setPosition({fields[i].box.position.x + 8.f, fields[i].box.position.y + 4.f});
                win.draw(val);
            }
        }

        // toggles
        drawBox(box_topmost, false);
        labelText(std::string("always_on_top: ") + (v_topmost ? "true" : "false"),
                  box_topmost.position.x + 8.f, box_topmost.position.y + 4.f, 16);
        drawBox(box_hideTB, false);
        labelText(std::string("hide_in_taskbar: ") + (v_hide_taskbar ? "true" : "false"),
                  box_hideTB.position.x + 8.f, box_hideTB.position.y + 4.f, 16);

        // buttons
        drawBtn(btn_ok, "OK");
        drawBtn(btn_cancel, "Cancel");

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

    // Settings
    SettingsManager settingsMgr("settings.txt");
    settingsMgr.applySettings();                     // load on start
    setAlwaysOnTop(win, Settings::always_on_top);    // honor setting immediately

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
    bool fontOk = font.openFromFile(FONT_PATH); // SFML 3: openFromFile

    // Text UI
    sf::Text titleText(font, "Voice Notes", 16);
    titleText.setFillColor(textCol);
    titleText.setPosition(sf::Vector2f(10.f, 8.f));

    // Buttons (+) and (×) and gear & mic (strings may change -> we'll re-layout every frame)
    sf::Text plus(font, "+", 20);         plus.setFillColor(textCol);
    sf::Text closeX(font, "x", 18);       closeX.setFillColor(textCol);
    sf::Text gear(font, u8"\u2699", 20);  gear.setFillColor(textCol);
    sf::Text microphone(font, "mic", 20); microphone.setFillColor(textCol);

    // Bounds updated each frame
    sf::FloatRect plusBounds, closeBounds, gearBounds, microphoneBounds;

    auto layoutHeader = [&](){
        // Right-aligned layout helper
        auto placeRight = [&](sf::Text& t, float& rightX, float pad = 8.f) {
            auto b = t.getLocalBounds();
            rightX -= (b.size.x + pad);
            t.setPosition({rightX, 6.f});
        };

        float rightX = static_cast<float>(HUB_W) - 6.f; // start near the right edge
        placeRight(closeX, rightX, 10.f);
        placeRight(plus,   rightX, 14.f);
        placeRight(gear,   rightX, 14.f);
        placeRight(microphone, rightX, 14.f);

        closeBounds      = closeX.getGlobalBounds();
        plusBounds       = plus.getGlobalBounds();
        gearBounds       = gear.getGlobalBounds();
        microphoneBounds = microphone.getGlobalBounds();
    };

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
        // Ensure header layout & hit boxes reflect any string changes (e.g., mic <-> stop)
        layoutHeader();

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
                        } else if (gearBounds.contains(mp)) {
                            // Temporarily drop top-most so settings can appear in front
                            bool prevTop = Settings::always_on_top;
                            setAlwaysOnTop(win, false);
                            if (openSettingsWindow(win, settingsMgr)) {
                                // re-apply settings that affect window behavior
                                setAlwaysOnTop(win, Settings::always_on_top);
                            } else {
                                // restore previous state on cancel/close
                                setAlwaysOnTop(win, prevTop);
                            }
                        } else if(microphoneBounds.contains(mp)) {
                            if(microphone.getString() == "mic") {
                                startRecordAudioFromMicrophone();
                                microphone.setString("stop");
                            } else {
                                stopRecordAudioFromMicrophone();
                                sendAudioFileToWhisper();
                                microphone.setString("mic");
                            }
                            // Re-layout since width changed
                            layoutHeader();
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
        if (fontOk) {
            win.draw(titleText);
            win.draw(plus);
            win.draw(closeX);
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

                if (fontOk) {
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
            if (fontOk) {
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
