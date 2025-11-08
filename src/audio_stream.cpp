#include <SFML/Audio/SoundBufferRecorder.hpp>
#include <iostream>
#include <string>
using namespace std;

int main() {
    bool recordLoop = true;
    string recordTextInput;

    // first check if an input audio device is available on the system
    if (!sf::SoundBufferRecorder::isAvailable())
    {
        // error: audio capture is not available on this system
        cout << "test";
    }
    
    // create the recorder
    sf::SoundBufferRecorder recorder;
    
    // start the capture
    recorder.start();
    
    // wait...
    while(recordLoop) {

        cout << "Enter Input when finished recording: ";
        cin >> recordTextInput;
        cout << recordTextInput;
        recordLoop = false;
    }
    
    // stop the capture
    recorder.stop();
    
    // retrieve the buffer that contains the captured audio data
    const sf::SoundBuffer& buffer = recorder.getBuffer();

    buffer.saveToFile("record.ogg");

    return 0;
}