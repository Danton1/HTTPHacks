# Voice Notes

## Project Overview
Take notes easily by recording your voice and automatically generate notes! 

C++ project to read audio streams and generate notes.

Uses SFML graphical and audio libraies api for frontend development and microphone capture.
Implements an open source port of OpenAI's whisper model using ggml for voice to text generation in C++. 

## Build & Run Instructions

### 1. Clone the Repository

    git clone https://github.com/Danton1/HTTPHacks.git
    cd HTTPHacks

---

### 2. Build the Whisper Model

Navigate to the model directory:

    cd whisper/models

Download and generate the model:

- On **macOS / Linux**:

      sh download-ggml-model.sh base.en

- On **Windows (PowerShell / CMD)**:

      download-ggml-model.cmd base.en

After the script finishes, you should see a model file such as  
`ggml-base.en.bin` inside the `whisper/models` folder.

---

### 3. Configure and Build the Application

Return to the project root:

    cd ../..

Run CMake to configure and build (https://cmake.org/download/):

    cmake -B build
    cmake --build build

---

### 4. Run the Program

Navigate to the executable:

    cd build/bin/Debug

Run:

    ./main.exe

---


## Project Members

- **Danton Soares**  
- **Conrad Christian**
