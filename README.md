<U>Project Overview</U><br>
C++ project to read audio streams and generate notes.

Uses SFML and CMake to build audio stream API.

---
<U>Build and Run the application</U>

Configure CMake: 
```
cmake -B build
```

Build with CMake:
```
cmake --build build
```

The executable will be in `build\bin\Debug`
Run to execute program:
```
.\build\bin\Debug\main.exe
```

To Build and Run the whisper model:
Inside the root of whisper.cpp
```
cd whisper.cpp
"./models/download-ggml-model.exe" base.en
cmake -B build
cmake --build build -j --config Release

// Testing
"./build/bin/Release/whisper-cli.exe" -f "samples/jfk.wav"
```
---

<U>Project Members</U><br>
Danton Soares<br>
Conrad Christian
