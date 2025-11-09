#include <SFML/Audio/SoundBufferRecorder.hpp>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
using namespace std;

int recordAudioFromMicrophone() {
    bool recordLoop = true;
    string recordTextInput;

    // first check if an input audio device is available on the system
    if (!sf::SoundBufferRecorder::isAvailable())
    {
        // error: audio capture is not available on this system
        cout << "audio capture device is not found";
    }
    
    // create the recorder
    sf::SoundBufferRecorder recorder;
    
    // start the capture
    recorder.start();
    
    // wait...
    while(recordLoop) {
        this_thread::sleep_for(chrono::seconds(3));

        recordLoop = false;
    }
    
    // stop the capture
    recorder.stop();
    
    // retrieve the buffer that contains the captured audio data
    const sf::SoundBuffer& buffer = recorder.getBuffer();

    buffer.saveToFile("record.ogg");

    return 0;
}