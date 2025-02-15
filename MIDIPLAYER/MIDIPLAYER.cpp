#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <string>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <windows.h>
#include <cstdio>
#include <conio.h>
#include "RtMidi.h"
#include "MidiFile.h"

using namespace smf;

std::atomic<bool> isPaused(false);
std::atomic<bool> isStopped(false);
std::atomic<bool> isMidiLoaded(false);
std::atomic<bool> isPlaybackFinished(false);
std::condition_variable cv;
std::condition_variable loadCv;
std::mutex mtx;

std::wstring to_wstring(const std::string& str) {
    int len = MultiByteToWideChar(CP_ACP, 0, str.c_str(), -1, NULL, 0);
    std::wstring wstr(len, 0);
    MultiByteToWideChar(CP_ACP, 0, str.c_str(), -1, &wstr[0], len);
    return wstr;
}

std::string openMidiFileDialog() {
    wchar_t filename[MAX_PATH] = { 0 };

    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFilter = L"MIDI Files\0*.mid;*.midi\0All Files\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle = L"Select a MIDI file";

    if (GetOpenFileNameW(&ofn)) {
        int len = WideCharToMultiByte(CP_ACP, 0, filename, -1, NULL, 0, NULL, NULL);
        std::string filepath(len, 0);
        WideCharToMultiByte(CP_ACP, 0, filename, -1, &filepath[0], len, NULL, NULL);
        return filepath;
    }

    return "";
}

void playMidiFile(const std::string& filePath, RtMidiOut& midiOut) {
    MidiFile midiFile;
    if (!midiFile.read(filePath)) {
        std::cerr << "Error: Failed to load MIDI file.\n";
        return;
    }

    // tempo 이벤트에 따른 실제 재생 시간(초) 계산
    midiFile.doTimeAnalysis();
    midiFile.linkNotePairs();

    // 모든 이벤트 수집 (event.seconds 기준)
    std::vector<const MidiEvent*> allEvents;
    for (int track = 0; track < midiFile.getTrackCount(); track++) {
        for (int j = 0; j < midiFile[track].size(); j++) {
            allEvents.push_back(&midiFile[track][j]);
        }
    }

    // event.seconds 기준 정렬 (tempo 변화 반영)
    std::sort(allEvents.begin(), allEvents.end(), [](const MidiEvent* a, const MidiEvent* b) {
        return a->seconds < b->seconds;
    });

    // 전체 재생 시간 (마지막 이벤트 시간)
    double totalDuration = allEvents.empty() ? 0.0 : allEvents.back()->seconds;

    {
        std::lock_guard<std::mutex> lock(mtx);
        isMidiLoaded = true;
    }
    loadCv.notify_one();

    std::cout << "Playing MIDI: " << filePath << std::endl;

    auto playbackStart = std::chrono::steady_clock::now();
    std::chrono::duration<double> pauseDuration = std::chrono::duration<double>::zero();
    std::chrono::steady_clock::time_point pauseStart{}; // 초기값

    // 각 이벤트 처리
    for (const MidiEvent* event : allEvents) {
        if (isStopped) break;

        double eventTime = event->seconds;
        auto targetTime = playbackStart + std::chrono::duration<double>(eventTime) + pauseDuration;

        while (true) {
            std::unique_lock<std::mutex> lock(mtx);
            if (isStopped) break;

            if (isPaused) {
                if (pauseStart == std::chrono::steady_clock::time_point{}) {
                    pauseStart = std::chrono::steady_clock::now();
                }
                cv.wait(lock, [] { return !isPaused || isStopped; });
                if (isStopped) break;
                auto now = std::chrono::steady_clock::now();
                pauseDuration += now - pauseStart;
                pauseStart = std::chrono::steady_clock::time_point{};
                targetTime = playbackStart + std::chrono::duration<double>(eventTime) + pauseDuration;
            }
            else {
                auto now = std::chrono::steady_clock::now();
                if (now >= targetTime) break;
                cv.wait_until(lock, targetTime, [] { return isPaused || isStopped; });
            }
        }

        // 콘솔 타이틀에 진행도 업데이트 (백분율)
        if (totalDuration > 0.0) {
            double progressPercent = (event->seconds / totalDuration) * 100.0;
            wchar_t title[256];
            swprintf_s(title, 256, L"현재 진행도: %.2f%%", progressPercent);
            SetConsoleTitleW(title);
        }

        if (event->isMeta() && (*event)[0] == 0x51) {
            int microsecondsPerQuarter = ((*event)[3] << 16) | ((*event)[4] << 8) | (*event)[5];
            double newBpm = 60000000.0 / microsecondsPerQuarter;
            std::cout << "⏱️ Tempo changed: " << newBpm << " BPM\n";
        }

        if (event->isNoteOn() || event->isNoteOff()) {
            std::vector<unsigned char> message = {
                static_cast<unsigned char>((*event)[0]),
                static_cast<unsigned char>((*event)[1]),
                static_cast<unsigned char>((*event)[2])
            };
            midiOut.sendMessage(&message);
        }
    }

    std::cout << "MIDI playback finished.\n";
    isPlaybackFinished = true;
    cv.notify_all();
}

int main() {
    try {
        RtMidiOut midiOut;
        if (midiOut.getPortCount() == 0) {
            std::cerr << "No available MIDI output ports.\n";
            return 1;
        }

        std::cout << "Available MIDI Ports:\n";
        for (unsigned int i = 0; i < midiOut.getPortCount(); i++) {
            std::cout << i << ": " << midiOut.getPortName(i) << "\n";
        }

        int portNumber;
        std::cout << "Select a MIDI port: ";
        std::cin >> portNumber;
        std::cin.ignore(); // 남은 개행 문자 제거

        try {
            midiOut.openPort(portNumber);
        }
        catch (RtMidiError& error) {
            std::cerr << "Failed to open MIDI port: " << error.getMessage() << "\n";
            return 1;
        }

        // 파일 재생 반복 루프
        while (true) {
            // 새 미디 재생을 위해 플래그 초기화
            isPaused = false;
            isStopped = false;
            isMidiLoaded = false;
            isPlaybackFinished = false;

            std::string filePath = openMidiFileDialog();
            if (filePath.empty()) {
                std::cerr << "No MIDI file selected.\n";
                break;
            }

            std::thread playbackThread(playMidiFile, filePath, std::ref(midiOut));

            // 미디가 로드될 때까지 기다림
            {
                std::unique_lock<std::mutex> lock(mtx);
                loadCv.wait(lock, [] { return isMidiLoaded.load(); });
            }

            std::cout << "\nCommands: [pause/resume/stop]\n";

            // 재생이 끝날 때까지 명령어 입력 처리 (_kbhit()로 non-blocking 체크)
            while (!isPlaybackFinished.load()) {
                if (_kbhit()) {
                    std::string command;
                    std::getline(std::cin, command);
                    if (command == "pause") {
                        isPaused = true;
                        cv.notify_all();
                        std::cout << "Paused\n";
                    }
                    else if (command == "resume") {
                        isPaused = false;
                        cv.notify_all();
                        std::cout << "Resumed\n";
                    }
                    else if (command == "stop") {
                        isStopped = true;
                        cv.notify_all();
                        std::cout << "Stopping playback...\n";
                        break;
                    }
                    else {
                        std::cout << "Invalid command. Use [pause/resume/stop]\n";
                    }
                }
                else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }

            playbackThread.join();

            std::cout << "\n다른 MIDI 파일을 재생하시겠습니까? (y/n): ";
            std::string answer;
            std::getline(std::cin, answer);
            if (answer != "y" && answer != "Y") {
                break;
            }
        }
    }
    catch (RtMidiError& error) {
        std::cerr << "MIDI error: " << error.getMessage() << "\n";
        return 1;
    }

    return 0;
}
