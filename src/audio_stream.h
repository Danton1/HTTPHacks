#ifndef AUDIO_STREAM_H
#define AUDIO_STREAM_H

#include <string>

int startRecordAudioFromMicrophone();
int stopRecordAudioFromMicrophone();
int sendAudioFileToWhisper(std::string audioPath, std::string textPath);

#endif