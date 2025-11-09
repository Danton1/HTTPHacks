#include "whisper.h"
#include <SFML/Audio/SoundBufferRecorder.hpp>
#include <SFML/Audio/SoundBuffer.hpp>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <vector>
#include <fstream>
#include "settings.h"

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

int sendAudioFileToWhisper(std::string audioPath, std::string textPath) {
    // Load the recorded WAV file saved by stopRecordAudioFromMicrophone()
    sf::SoundBuffer buffer;
    if (!buffer.loadFromFile(audioPath)) {
        std::cerr << "Failed to load record.wav\n";
        return 1;
    }

    auto samples16 = buffer.getSamples();
    const std::size_t sampleCount = buffer.getSampleCount(); // total samples (frames * channels)
    const unsigned sampleRate = buffer.getSampleRate();
    const unsigned channelCount = buffer.getChannelCount();

    // Convert to mono float [-1,1], downmix if needed (at the source sample rate)
    const std::size_t frames = sampleCount / channelCount;
    std::vector<float> pcmf32;
    pcmf32.resize(frames);

    for (std::size_t i = 0; i < frames; ++i) {
        int32_t acc = 0;
        for (unsigned ch = 0; ch < channelCount; ++ch) {
            acc += samples16[i * channelCount + ch];
        }
        float f = static_cast<float>(acc) / (static_cast<float>(channelCount) * 32768.0f);
        if (f > 1.0f) f = 1.0f;
        if (f < -1.0f) f = -1.0f;
        pcmf32[i] = f;
    }

    // If sample rate differs, resample to WHISPER_SAMPLE_RATE using linear interpolation
    std::vector<float> pcmf32_resampled;
    if (sampleRate != WHISPER_SAMPLE_RATE) {
        const int in_rate = static_cast<int>(sampleRate);
        const int out_rate = WHISPER_SAMPLE_RATE;
        const std::size_t in_frames = pcmf32.size();
        if (in_frames == 0) {
            std::cerr << "No audio frames to resample\n";
            return 2;
        }

        const std::size_t out_frames = static_cast<std::size_t>( (double)in_frames * out_rate / in_rate + 0.5 );
        pcmf32_resampled.resize(out_frames);

        const double rate_ratio = static_cast<double>(in_rate) / static_cast<double>(out_rate);
        for (std::size_t i = 0; i < out_frames; ++i) {
            double src = i * rate_ratio;
            std::size_t idx = static_cast<std::size_t>(std::floor(src));
            double frac = src - (double)idx;
            float v0 = pcmf32[idx];
            float v1 = (idx + 1 < in_frames) ? pcmf32[idx + 1] : v0;
            pcmf32_resampled[i] = static_cast<float>(v0 * (1.0 - frac) + v1 * frac);
        }
        std::cout << "Resampled audio: " << in_frames << " -> " << out_frames << " frames (" << sampleRate << " -> " << WHISPER_SAMPLE_RATE << " Hz)\n";
    } else {
        pcmf32_resampled.swap(pcmf32);
    }

    // Initialize model/context
    whisper_context_params cparams = whisper_context_default_params();
    struct whisper_context * ctx = whisper_init_from_file_with_params("whisper/models/ggml-base.en.bin", cparams);
    if (!ctx) {
        std::cerr << "Failed to load model 'whisper/models/ggml-base.en.bin'\n";
        return 3;
    }

    struct whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.n_threads = 4; // TODO: tune as appropriate

    std::cout << "starting whisper transcription\n";

    // run transcription
    const int n_samples = static_cast<int>(pcmf32_resampled.size());
    if (whisper_full(ctx, wparams, pcmf32_resampled.data(), n_samples) != 0) {
        std::fprintf(stderr, "whisper_full failed\n");
        whisper_free(ctx);
        return 5;
    }

    std::cout << "transcription completed, writing text file\n";

    // print segments to text file
    ofstream audioTextFile;
    audioTextFile.open(textPath);
    const int n_segments = whisper_full_n_segments(ctx);
    for (int i = 0; i < n_segments; ++i) {
        const char *seg_text = whisper_full_get_segment_text(ctx, i);
        audioTextFile << (seg_text ? seg_text : "") << '\n';
    }
    
    audioTextFile.close();
    whisper_free(ctx);
    return 0;
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

    if (!Settings::audio_input_device.empty()) {
        recorder.setDevice(Settings::audio_input_device); // ignore failure -> SFML will keep current
    }
    // start the capture

    if (!recorder.start()) {
        // error: failed to start audio capture
        cout << "failed to start audio capture";
        return 1;
    }

    cout << "Recording..." << endl;

    return 0;
}

int stopRecordAudioFromMicrophone() {    
    // stop the capture
    recorder.stop();
    
    // retrieve the buffer that contains the captured audio data
    const sf::SoundBuffer& buffer = recorder.getBuffer();

    // Save .wav
    string baseName = "note_" + getTimestamp();
    std::string dir = Settings::voice_notes_path;
    if (!dir.empty() && (dir.back() != '/' && dir.back() != '\\')) dir.push_back('/');
    std::string audioPath = dir + baseName + ".wav";
    std::string textPath  = dir + baseName + ".txt";


    std::filesystem::create_directories(Settings::voice_notes_path);
    if (!buffer.saveToFile(audioPath)) {
        cerr << "Failed to save audio.\n";
        return 1;
    }
    cout << "Saved: " << audioPath << "\n";

    // Save empty txt file
    ofstream out(textPath);
    cout<<"Saved: " << textPath << "\n";

    sendAudioFileToWhisper(audioPath, textPath);

    return 0;
}