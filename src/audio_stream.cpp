#include <SFML/Audio/SoundBufferRecorder.hpp>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
using namespace std;

sf::SoundBufferRecorder recorder;

int startRecordAudioFromMicrophone() {
    bool recordLoop = true;
    string recordTextInput;

    // first check if an input audio device is available on the system
    if (!sf::SoundBufferRecorder::isAvailable())
    {
        // error: audio capture is not available on this system
        cout << "audio capture device is not found";
    }
    
    // start the capture
    recorder.start();

    return 0;
}

int stopRecordAudioFromMicrophone() {    
    // stop the capture
    recorder.stop();
    
    // retrieve the buffer that contains the captured audio data
    const sf::SoundBuffer& buffer = recorder.getBuffer();

    buffer.saveToFile("record.wav");

    return 0;
}