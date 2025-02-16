#define _WIN32_WINNT 0x0A00

#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <string>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <atomic>
#include <iomanip>
#include <stdexcept>
#include <cctype>
#include <regex>
#include <array>
#include <condition_variable>
#include <cstdio>
#include <conio.h>
#include "RtMidi.h"
#include "MidiFile.h"
#ifdef _WIN32
#include <windows.h>
#endif

using namespace smf;

std::atomic<double> currentBpm(120.0);
std::atomic<bool> isPaused(false);
std::atomic<bool> isStopped(false);
std::atomic<bool> isMidiLoaded(false);
std::atomic<bool> isPlaybackFinished(false);
std::condition_variable cv;
std::condition_variable loadCv;
std::mutex mtx;
std::atomic<int> globalNoteCount(0);
std::atomic<double> currentPlaybackTime(0.0);

void SetColor(WORD color) {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(hConsole, color);
}

std::string cleanVersionString(const std::string& version) {
    std::string cleaned = version;
    cleaned.erase(std::remove_if(cleaned.begin(), cleaned.end(), [](unsigned char c) {
        return !(std::isdigit(c) || c == '.');
        }), cleaned.end());
    return cleaned;
}

std::string execCommand(const std::string& cmd) {
    std::array<char, 128> buffer;
    std::string result;
    FILE* pipe = _popen(cmd.c_str(), "r");
    if (!pipe) {
        throw std::runtime_error("[!] popen() Failure");
    }
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }
    _pclose(pipe);
    return result;
}

#ifdef _WIN32
// Function to check a specific registry key to determine if Visual C++ Redistributable is installed.
bool isVCRedistInstalled(const std::wstring& subKey) {
    HKEY hKey;
    LONG lRes = RegOpenKeyExW(HKEY_LOCAL_MACHINE, subKey.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &hKey);
    if (lRes != ERROR_SUCCESS) {
        lRes = RegOpenKeyExW(HKEY_LOCAL_MACHINE, subKey.c_str(), 0, KEY_READ | KEY_WOW64_32KEY, &hKey);
    }
    if (lRes == ERROR_SUCCESS) {
        DWORD installed = 0;
        DWORD size = sizeof(installed);
        LONG lRes2 = RegQueryValueExW(hKey, L"Installed", NULL, NULL, reinterpret_cast<LPBYTE>(&installed), &size);
        RegCloseKey(hKey);
        if (lRes2 == ERROR_SUCCESS && installed == 1)
            return true;
    }
    return false;
}

// Check if Visual C++ 2015 Redistributable (x86 or x64) is installed
bool isVisualCppRedistributableInstalled() {
    bool installed_x86 = isVCRedistInstalled(L"SOFTWARE\\Microsoft\\VisualStudio\\14.0\\VC\\Runtimes\\x86");
    bool installed_x64 = isVCRedistInstalled(L"SOFTWARE\\Microsoft\\VisualStudio\\14.0\\VC\\Runtimes\\x64");
    return installed_x86 || installed_x64;
}
#endif


typedef NTSTATUS(NTAPI* RtlGetVersionPtr)(POSVERSIONINFOEXW);

bool IsWindows10OrGreater() {
    HMODULE hModule = LoadLibrary(L"ntdll.dll");
    if (hModule == NULL) {
        SetColor(12);
        std::cerr << "[!] Failed to load ntdll.dll" << std::endl;
        return false;
    }

    RtlGetVersionPtr pRtlGetVersion = (RtlGetVersionPtr)GetProcAddress(hModule, "RtlGetVersion");
    if (pRtlGetVersion == NULL) {
        SetColor(12);
        std::cerr << "[!] Failed to get RtlGetVersion function address" << std::endl;
        FreeLibrary(hModule);
        return false;
    }

    OSVERSIONINFOEXW versionInfo;
    ZeroMemory(&versionInfo, sizeof(versionInfo));

    versionInfo.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEXW);

    NTSTATUS status = pRtlGetVersion(&versionInfo);
    if (status != 0) {
        SetColor(12);
        std::cerr << "[!] Failed to get version info" << std::endl;
        FreeLibrary(hModule);
        return false;
    }
    if (versionInfo.dwMajorVersion >= 10) {
        FreeLibrary(hModule);
        return true;
    }

    FreeLibrary(hModule);
    return false;
}

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
        SetColor(12);
        std::cerr << "[!] Failed to load MIDI file.\n";
        return;
    }

    midiFile.doTimeAnalysis();
    midiFile.linkNotePairs();

    std::vector<const MidiEvent*> allEvents;
    for (int track = 0; track < midiFile.getTrackCount(); track++) {
        for (int j = 0; j < midiFile[track].size(); j++) {
            allEvents.push_back(&midiFile[track][j]);
        }
    }

    std::sort(allEvents.begin(), allEvents.end(), [](const MidiEvent* a, const MidiEvent* b) {
        return a->seconds < b->seconds;
        });

    double totalDuration = allEvents.empty() ? 0.0 : allEvents.back()->seconds;
    int totalNotes = 0;
    int minutes = static_cast<int>(totalDuration) / 60;
    double seconds = totalDuration - minutes * 60;
    for (const auto& event : allEvents) {
        if (event->isNoteOn()) totalNotes++;
    }
    SetColor(15);
    std::cout << "\n[ MIDI Information ]" << std::endl;
    SetColor(11);
    std::cout << "  Playing MIDI: " << filePath << "\n"
        << "  Total Notes: " << totalNotes << "\n"
        << "  Duration: " << minutes << "m "
        << std::fixed << std::setprecision(2) << seconds << "s\n";

    {
        std::lock_guard<std::mutex> lock(mtx);
        isMidiLoaded = true;
    }
    loadCv.notify_one();


    auto playbackStart = std::chrono::steady_clock::now();
    std::chrono::duration<double> pauseDuration = std::chrono::duration<double>::zero();
    std::chrono::steady_clock::time_point pauseStart{};

    int noteCount = 0;
    double lastEventTime = 0.0;

    // Thread that updates console title every second
    std::thread titleUpdater([totalDuration]() {
        while (!isPlaybackFinished) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            int notes = globalNoteCount.exchange(0);
            double cpTime = currentPlaybackTime.load();
            double progressPercent = (cpTime / totalDuration) * 100.0;

            wchar_t title[256];
            swprintf_s(title, 256,
                L"Progress: %.2f%% | NPS: %d | BPM: %.1f",
                progressPercent, notes, currentBpm.load()
            );
            SetConsoleTitleW(title);
        }
        });

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

        // Update playback time (for title update)
        currentPlaybackTime.store(eventTime);

        // [Existing title update code - commented out as it doesn't update every second]
        /*
        if (totalDuration > 0.0) {
            double progressPercent = (event->seconds / totalDuration) * 100.0;
            double nps = (eventTime - lastEventTime > 0) ? noteCount / (eventTime - lastEventTime) : 0.0;
            wchar_t title[256];
            swprintf_s(title, 256, L\"Progress: %.2f%% | NPS: %.2f\", progressPercent, nps);
            SetConsoleTitleW(title);
            lastEventTime = eventTime;
            noteCount = 0;
        }
        */

        if (event->isMeta() && (*event)[0] == 0x51) {
            int mpq = ((*event)[3] << 16) | ((*event)[4] << 8) | (*event)[5];
            currentBpm.store(60000000.0 / mpq);
        }

        if (event->isNoteOn()) {
            noteCount++;    
            globalNoteCount++;   
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

    SetColor(13);
    std::cout << "[*] MIDI playback finished.\n";
    isPlaybackFinished = true;
    cv.notify_all();

    if (titleUpdater.joinable()) {
        titleUpdater.join();
    }
}


int main() {
    try {
        if (IsWindows10OrGreater()) {
            SetColor(6);
            std::cout << "[*] Checking dependencies...\n" << std::endl;
            if (isVisualCppRedistributableInstalled()) {
                SetColor(10);
                std::cout << "[+] Visual C++ Redistributable already installed" << std::endl;
            }
            else {
                SetColor(12);
                std::cout << "[!] Visual C++ Redistributable is not installed" << std::endl;
            }
            SetColor(6);
            std::string localFilePath = "VERSION";
            std::string remoteUrl = "https://raw.githubusercontent.com/MatrixMTX/MidiPlayer/refs/heads/main/MIDIPLAYER/VERSION";
            std::cout << "[*] Fetching version information..." << std::endl;
            std::ifstream localFile(localFilePath);
            if (!localFile) {
                SetColor(12);
                std::cerr << "[!] Failed to Check Version" << std::endl;
                SetColor(15);
                std::cout << "Press Enter to exit..." << std::endl;
                std::cin.get();
                return 0;
            }
            std::stringstream localBuffer;
            localBuffer << localFile.rdbuf();
            std::string localVer = localBuffer.str();
            std::string localVersion = cleanVersionString(localVer);
            if (localVersion.empty()) {
                SetColor(12);
                std::cerr << "[!] Could not find version information in local file." << std::endl;
                SetColor(15);
            }
            std::string command = "powershell -Command (Invoke-WebRequest -Uri '" + remoteUrl + "').Content";
            std::string remoteVer;
            try {
                remoteVer = execCommand(command);
            }
            catch (const std::exception& ex) {
                SetColor(12);
                std::cerr << "[!] Command execution error: " << ex.what() << std::endl;
                SetColor(15);
                std::cout << "Press Enter to exit..." << std::endl;
                std::cin.get();
                return 1;
            }
            if (remoteVer.empty()) {
                SetColor(12);
                std::cerr << "[!] Failed to retrieve remote file contents: " << remoteUrl << std::endl;
                SetColor(15);
                std::cout << "Press Enter to exit..." << std::endl;
                std::cin.get();
                return 1;
            }
            std::string remoteVersion = cleanVersionString(remoteVer);
            if (remoteVersion.empty()) {
                SetColor(12);
                std::cerr << "[!] Could not find version information on remote file." << std::endl;
                SetColor(15);
                std::cout << "Press Enter to exit..." << std::endl;
                std::cin.get();
                return 1;
            }
            std::cout << "[+] Version information fetched successfully!" << std::endl;

            SetColor(15);
            std::cout << "\n[ Version Information ]" << std::endl;
            SetColor(11);
            std::cout << "  Program Version: " << localVersion << std::endl;
            std::cout << "  Legacy Version: " << remoteVersion << std::endl;

            if (localVersion == remoteVersion) {
                SetColor(6);
                std::cout << "\n[!] The current file version is the latest." << std::endl;
            }
            else {
                SetColor(6);
                std::cout << "\n[!] The current file version is not up to date." << std::endl;
            }
        }
        else {
            SetColor(12);
            std::cout << "[!] This program requires Windows 10 or greater." << std::endl;
            SetColor(15);
            std::cout << "Press Enter to exit..." << std::endl;
            std::cin.get();
            return 0;
        }

        RtMidiOut midiOut;
        if (midiOut.getPortCount() == 0) {
            SetColor(12);
            std::cerr << "[!] No available MIDI output ports.\n";
            return 1;
        }

        SetColor(10);
        std::cout << "[*] Available MIDI Ports:\n";
        for (unsigned int i = 0; i < midiOut.getPortCount(); i++) {
            std::cout << i << ": " << midiOut.getPortName(i) << "\n";
        }

        SetColor(11);
        int portNumber;
        std::cout << "\nSelect a MIDI port: ";
        std::cin >> portNumber;
        std::cin.ignore();

        try {
            midiOut.openPort(portNumber);
        }
        catch (RtMidiError& error) {
            SetColor(12);
            std::cerr << "[!] Failed to open MIDI port: " << error.getMessage() << "\n";
            return 1;
        }

        while (true) {
            isPaused = false;
            isStopped = false;
            isMidiLoaded = false;
            isPlaybackFinished = false;

            std::string filePath = openMidiFileDialog();
            if (filePath.empty()) {
                SetColor(12);
                std::cerr << "[!] No MIDI file selected.\n";
                break;
            }

            std::thread playbackThread(playMidiFile, filePath, std::ref(midiOut));

            {
                std::unique_lock<std::mutex> lock(mtx);
                loadCv.wait(lock, [] { return isMidiLoaded.load(); });
            
            }
                
            SetColor(11);
            std::cout << "\nCommands (pause/resume/stop): ";

            while (!isPlaybackFinished.load()) {
                if (_kbhit()) {
                    std::string command;
                    std::getline(std::cin, command);
                    if (command == "pause") {
                        isPaused = true;
                        cv.notify_all();
                        SetColor(10);
                        std::cout << "[*] Paused\n";
                        SetColor(11);
                        std::cout << "Commands (pause/resume/stop): ";
                    }
                    else if (command == "resume") {
                        isPaused = false;
                        cv.notify_all();
                        SetColor(10);
                        std::cout << "[*] Resumed\n";
                        SetColor(11);
                        std::cout << "Commands (pause/resume/stop): ";
                    }
                    else if (command == "stop") {
                        isStopped = true;
                        cv.notify_all();
                        SetColor(10);
                        std::cout << "[*] Stopping playback...\n";
                        SetColor(11);
                        std::cout << "Commands (pause/resume/stop): ";
                        break;
                    }
                    else {
                        SetColor(12);
                        std::cout << "[!] Invalid command. Use [pause/resume/stop]\n";
                        SetColor(11);
                        std::cout << "Commands (pause/resume/stop): ";
                    }
                }
                else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }

            playbackThread.join();
            SetColor(13);
            std::cout << "\n[*] Would you like to play another MIDI file? (y/n): ";
            std::string answer;
            std::getline(std::cin, answer);
            if (answer != "y" && answer != "Y") {
                SetColor(15);
                std::cout << "Press Enter to exit...";
                std::cin.get();
                break;
            }
        }
    }
    catch (RtMidiError) {
        SetColor(12);
        std::cerr << "[!] An error occurred. Please try again later." << "\n";
        SetColor(15);
        std::cout << "Press Enter to exit..." << std::endl;
        std::cin.get();
        return 1;
    }

    return 0;
}
