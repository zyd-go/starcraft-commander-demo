// Native StarCraft replay controls. This module issues no unit commands.
#include <windows.h>
#include <BWAPI.h>
#include "MatchIdentity.h"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <string>

class ReplayControl final : public BWAPI::AIModule {
    static inline ReplayControl* active = nullptr;
    UINT_PTR timerId = 0;
    unsigned long long sequence = 0;
    ULONGLONG lastRead = 0, lastWrite = 0;
    int target = 0, speed = 42;
    bool paused = false, seeking = false, ended = false, reload = false;

    static void CALLBACK timer(HWND, UINT, UINT_PTR, DWORD) {
        if (active) active->update();
    }

    static int setting(const char* key, int fallback) {
        const char* value = std::getenv(key);
        return value ? std::atoi(value) : fallback;
    }

    void publish() {
        std::ofstream output("bwapi-data/write/replay-status.tmp", std::ios::trunc);
        output << "{\"frame\":" << BWAPI::Broodwar->getFrameCount()
               << ",\"total\":" << BWAPI::Broodwar->getReplayFrameCount()
               << ",\"target\":" << target
               << ",\"paused\":" << (paused ? "true" : "false")
               << ",\"seeking\":" << (seeking ? "true" : "false")
               << ",\"ended\":" << (ended ? "true" : "false")
               << ",\"reload\":" << (reload ? "true" : "false")
               << ",\"sequence\":" << sequence << "}\n";
        output.close();
        MoveFileExA("bwapi-data/write/replay-status.tmp",
                    "bwapi-data/write/replay-status.json", MOVEFILE_REPLACE_EXISTING);
        lastWrite = GetTickCount64();
    }

    void applyPlayback() {
        auto* game = BWAPI::BroodwarPtr;
        if (seeking) {
            game->setLocalSpeed(0);
            game->setFrameSkip(16);
            if (game->isPaused()) game->resumeGame();
        } else {
            game->setFrameSkip(1);
            game->setLocalSpeed(speed);
            if (paused && !game->isPaused()) game->pauseGame();
            if (!paused && game->isPaused()) game->resumeGame();
        }
    }

public:
    void onStart() override {
        if (!BWAPI::Broodwar->isReplay()) {
            BWAPI::Broodwar->leaveGame();
            return;
        }
        MatchIdentity::save(BWAPI::BroodwarPtr);
        target = std::clamp(setting("REPLAY_INITIAL_FRAME", 0), 0,
                            std::max(0, BWAPI::Broodwar->getReplayFrameCount() - 1));
        paused = setting("REPLAY_INITIAL_PAUSED", 0) != 0;
        speed = std::clamp(setting("REPLAY_INITIAL_SPEED", 42), 1, 168);
        seeking = target > 0;
        applyPlayback();
        active = this;
        // Replay pause can suppress AIModule frame callbacks. Poll on the same
        // game thread so controls remain responsive while native playback is paused.
        timerId = SetTimer(nullptr, 0, 80, timer);
        publish();
    }

    void onFrame() override { update(); }

    void update() {
        auto* game = BWAPI::BroodwarPtr;
        if (!game->isReplay() || ended) return;
        const auto now = GetTickCount64();
        if (now - lastRead >= 80) {
            lastRead = now;
            std::ifstream input("bwapi-data/read/replay-command.txt");
            unsigned long long incoming = 0;
            std::string action;
            int frame = 0, pauseValue = 0, requestedSpeed = 42;
            if (input >> incoming >> action >> frame >> pauseValue >> requestedSpeed &&
                incoming > sequence) {
                sequence = incoming;
                paused = pauseValue != 0;
                speed = std::clamp(requestedSpeed, 1, 168);
                if (action == "seek") {
                    target = std::clamp(frame, 0, std::max(0, game->getReplayFrameCount() - 1));
                    reload = target < game->getFrameCount();
                    seeking = !reload && target > game->getFrameCount();
                }
                applyPlayback();
                publish();
            }
        }
        if (seeking && game->getFrameCount() >= target) {
            seeking = false;
            applyPlayback();
            publish();
        }
        if (seeking && target - game->getFrameCount() < 128) game->setFrameSkip(1);
        if (!seeking && !reload && game->isPaused() != paused) applyPlayback();
        if (now - lastWrite >= 150) publish();
    }

    void onEnd(bool) override {
        if (timerId) { KillTimer(nullptr, timerId); timerId = 0; }
        active = nullptr;
        ended = true;
        seeking = false;
        publish();
    }
};

extern "C" void gameInit(BWAPI::Game* game) { BWAPI::BroodwarPtr = game; }
extern "C" BWAPI::AIModule* newAIModule() { return new ReplayControl(); }
