#include <SFML/Audio/SoundBufferRecorder.hpp>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
using namespace std;

string getTimestamp() {
    time_t now = time(nullptr);
    tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    local = *localtime(&now);
#endif
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d_%H-%M-%S", &local);
    return string(buf);
}

sf::SoundBufferRecorder recorder;

int startRecordAudioFromMicrophone() {
    bool recordLoop = true;
    string recordTextInput;

    // first check if an input audio device is available on the system
    if (!sf::SoundBufferRecorder::isAvailable())
    {
        // error: audio capture is not available on this system
        cout << "audio capture device is not found";
        return 1;
    }
    
    // start the capture
    recorder.start();

    cout << "Recording..." << endl;

    return 0;
}

int stopRecordAudioFromMicrophone() {    
    // stop the capture
    recorder.stop();
    
    // retrieve the buffer that contains the captured audio data
    const sf::SoundBuffer& buffer = recorder.getBuffer();

    buffer.saveToFile("record.wav");

    // Save .wav
    string baseName = "note_" + getTimestamp();
    string audioPath = "voice_notes/" + baseName + ".wav";
    string textPath = "voice_notes/" + baseName + ".txt";

    filesystem::create_directories("voice_notes");
    if (!buffer.saveToFile(audioPath)) {
        cerr << "Failed to save audio.\n";
        return 1;
    }

    return 0;
}