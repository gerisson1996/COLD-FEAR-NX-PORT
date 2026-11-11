// The PC build's sound: the game's 5.1 mix folded to stereo as the Switch does (host-switch/audio.cpp), played
// through XAudio2 in 10 ms blocks, six queued at most
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xaudio2.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "audio/output.h"
#include "core/log.h"

namespace audio {

namespace {

constexpr float kCentreFold = 0.707f;
constexpr uint32_t kBlock = kRate / 100;
constexpr int kBuffers = 6;

std::atomic<uint32_t> g_mode{kOutputAuto};
std::atomic<bool> g_paused{false};

IXAudio2* g_xa = nullptr;
IXAudio2MasteringVoice* g_master = nullptr;
IXAudio2SourceVoice* g_voice = nullptr;

void Fold(const float* in, int16_t* out, bool mono) {
    for (uint32_t i = 0; i < kBlock; i++) {
        const float* f = in + size_t(i) * kChannels;
        float c = f[Centre] * kCentreFold;
        float l = f[FrontLeft] + c + f[BackLeft];
        float r = f[FrontRight] + c + f[BackRight];
        if (mono) l = r = (l + r) * 0.5f;
        out[i * 2] = int16_t(std::clamp(l, -1.0f, 1.0f) * 32767.0f);
        out[i * 2 + 1] = int16_t(std::clamp(r, -1.0f, 1.0f) * 32767.0f);
    }
}

void Run(std::function<void(float*, uint32_t)> render) {
    std::vector<float> mix(size_t(kBlock) * kChannels);
    std::vector<int16_t> pcm(size_t(kBlock) * 2 * kBuffers);
    int next = 0;
    for (;;) {
        XAUDIO2_VOICE_STATE st{};
        g_voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (st.BuffersQueued >= kBuffers - 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        int16_t* out = pcm.data() + size_t(next) * kBlock * 2;
        if (g_paused) {
            std::memset(out, 0, size_t(kBlock) * 2 * sizeof(int16_t));
        } else {
            render(mix.data(), kBlock);
            Fold(mix.data(), out, g_mode == kOutputMono);
        }
        XAUDIO2_BUFFER b{};
        b.AudioBytes = kBlock * 2 * sizeof(int16_t);
        b.pAudioData = reinterpret_cast<const BYTE*>(out);
        g_voice->SubmitSourceBuffer(&b);
        next = (next + 1) % kBuffers;
    }
}

}  // namespace

bool IsSurround() { return g_mode == kOutputSurround; }
bool IsMono() { return g_mode == kOutputMono; }

void SetOutputMode(uint32_t mode) {
    if (g_mode.exchange(mode) != mode) LOGI("audio: output %u", mode);
}

void SetPaused(bool paused) { g_paused = paused; }

void StartOutput(std::function<void(float*, uint32_t)> render) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(XAudio2Create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR)) ||
        FAILED(g_xa->CreateMasteringVoice(&g_master, 2, kRate))) {
        LOGE("audio: XAudio2 unavailable, no sound");
        return;
    }
    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = kRate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = kRate * 4;
    if (FAILED(g_xa->CreateSourceVoice(&g_voice, &wf))) {
        LOGE("audio: no XAudio2 voice, no sound");
        return;
    }
    g_voice->Start();
    std::thread(Run, std::move(render)).detach();
    LOGI("audio: stereo through XAudio2 (PC build)");
}

}  // namespace audio
