// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace {

// Leading fields from the scePadReadState layout used by SwanStationPS5.
// Reserve room for the rest of the record, including extension and device data.
struct alignas(8) PadState {
    uint32_t buttons;
    uint8_t leftX;
    uint8_t leftY;
    uint8_t reserved[70];
    uint8_t connected;
    uint8_t tail[435];
};
static_assert(sizeof(PadState) == 512);
static_assert(offsetof(PadState, connected) == 76);

extern "C" {
int sceUserServiceInitialize(const void *params);
int sceUserServiceGetInitialUser(int *user);
int sceUserServiceTerminate();
int scePadInit();
int scePadOpen(int user, int type, int index, const void *params);
int scePadReadState(int handle, PadState *data);
int scePadClose(int handle);
int sceAudioOutInit();
int sceAudioOutOpen(int user, int type, int index, unsigned length, unsigned rate, unsigned format);
int sceAudioOutOutput(int handle, const void *data);
int sceAudioOutClose(int handle);
}

constexpr unsigned kSourceRate = 44100;
constexpr unsigned kOutputRate = 48000;

// Short, soft cues with different contours for navigation, confirmation and back.
auto MakeMenuCue(float firstHz, float secondHz, unsigned durationMs) {
    std::vector<int16_t> samples(kOutputRate * durationMs / 1000);
    float phase = 0.0f;
    for (size_t i = 0; i < samples.size(); ++i) {
        const float position = float(i) / float(samples.size() - 1);
        const float time = float(i) / kOutputRate;
        const float envelope = std::min(1.0f, time / 0.004f) * (1.0f - position) * (1.0f - position);
        const float tone = std::sin(phase) + 0.16f * std::sin(2.0f * phase);
        samples[i] = static_cast<int16_t>(4100.0f * envelope * tone);
        phase += 2 * std::numbers::pi_v<float> * std::lerp(firstHz, secondHz, position) / kOutputRate;
    }
    return samples;
}

} // namespace

namespace ps5 {

Pad::~Pad() {
    if (m_handle >= 0) {
        scePadClose(m_handle);
    }
    if (m_userService) {
        sceUserServiceTerminate();
    }
}

bool Pad::Open() {
    m_userService = sceUserServiceInitialize(nullptr) >= 0;
    int user = -1;
    if (sceUserServiceGetInitialUser(&user) < 0) {
        return false;
    }
    scePadInit();
    m_handle = scePadOpen(user, 0, 0, nullptr);
    return m_handle >= 0;
}

uint32_t Pad::Read() {
    PadState state{};
    if (m_handle < 0 || scePadReadState(m_handle, &state) < 0 || !state.connected) {
        return 0;
    }

    uint32_t buttons = state.buttons;
    if (state.leftX < 64) {
        buttons |= Left;
    } else if (state.leftX > 192) {
        buttons |= Right;
    }
    if (state.leftY < 64) {
        buttons |= Up;
    } else if (state.leftY > 192) {
        buttons |= Down;
    }
    return buttons;
}

Audio::~Audio() {
    {
        const std::lock_guard lock{m_mutex};
        m_stopping = true;
    }
    m_ready.notify_one();
    if (m_worker.joinable()) {
        m_worker.join();
    }
    if (m_handle >= 0) {
        sceAudioOutClose(m_handle);
    }
}

bool Audio::Open() {
    // Port type 0 is the main output. Format 1 is interleaved stereo PCM16.
    sceAudioOutInit();
    m_handle = sceAudioOutOpen(0xff, 0, 0, kGrainSize, kOutputRate, 1);
    if (m_handle < 0) {
        return false;
    }
    m_worker = std::thread{&Audio::Playback, this};
    return true;
}

void Audio::PushSample(int16_t left, int16_t right) {
    // Keep the resampler phase continuous across emulated frames.
    constexpr unsigned inputStep = kSourceRate / 300;
    constexpr unsigned outputStep = kOutputRate / 300;
    while (m_phase < outputStep) {
        const auto interpolate = [this](int from, int to) {
            return static_cast<int16_t>(from + (to - from) * static_cast<int>(m_phase) / int(outputStep));
        };
        QueueSample(interpolate(m_previousLeft, left), interpolate(m_previousRight, right));
        m_phase += inputStep;
    }
    m_phase -= outputStep;
    m_previousLeft = left;
    m_previousRight = right;
}

void Audio::Clear() {
    m_phase = 0;
    m_pendingSamples = 0;
    m_previousLeft = 0;
    m_previousRight = 0;
    const std::lock_guard lock{m_mutex};
    m_head = 0;
    m_count = 0;
}

void Audio::SetSettings(const AudioSettings &settings) {
    const std::lock_guard lock{m_mutex};
    m_settings = settings;
    if (settings.mute || settings.menuVolume == 0) {
        m_menuSound = {};
    }
}

void Audio::PlayMenuSound(MenuSound sound) {
    if (m_handle < 0) {
        return;
    }
    static const std::array cues{MakeMenuCue(670.0f, 690.0f, 42),
                                 MakeMenuCue(610.0f, 960.0f, 106),
                                 MakeMenuCue(730.0f, 460.0f, 90)};
    {
        const std::lock_guard lock{m_mutex};
        if (m_settings.mute || m_settings.menuVolume == 0) {
            return;
        }
        m_menuSound = cues[unsigned(sound)];
    }
    m_ready.notify_one();
}

void Audio::QueueSample(int16_t left, int16_t right) {
    m_pending[m_pendingSamples++] = left;
    m_pending[m_pendingSamples++] = right;
    if (m_pendingSamples != m_pending.size()) {
        return;
    }
    m_pendingSamples = 0;
    {
        const std::lock_guard lock{m_mutex};
        if (m_count == m_queue.size()) {
            m_head = (m_head + 1) % m_queue.size();
            --m_count;
        }
        m_queue[(m_head + m_count) % m_queue.size()] = m_pending;
        ++m_count;
    }
    m_ready.notify_one();
}

void Audio::Playback() {
    {
        std::unique_lock lock{m_mutex};
        m_ready.wait(lock, [this] { return m_count > 0 || !m_menuSound.empty() || m_stopping; });
    }

    for (;;) {
        Grain grain{};
        AudioSettings settings;
        std::span<const int16_t> effect;
        {
            const std::lock_guard lock{m_mutex};
            if (m_stopping) {
                return;
            }
            if (m_count > 0) {
                grain = m_queue[m_head];
                m_head = (m_head + 1) % m_queue.size();
                --m_count;
            }
            settings = m_settings;
            effect = m_menuSound.first(std::min<size_t>(kGrainSize, m_menuSound.size()));
            m_menuSound = m_menuSound.subspan(effect.size());
        }
        if (settings.mute) {
            grain.fill(0);
        } else if (settings.gameVolume != 100 || !effect.empty()) {
            for (size_t i = 0; i < kGrainSize; ++i) {
                const int cue = i < effect.size() ? effect[i] * int(settings.menuVolume) / 100 : 0;
                for (size_t j = i * 2; j < i * 2 + 2; ++j) {
                    const int sample = grain[j] * int(settings.gameVolume) / 100 + cue;
                    grain[j] = static_cast<int16_t>(std::clamp(sample, -32768, 32767));
                }
            }
        }
        if (sceAudioOutOutput(m_handle, grain.data()) < 0) {
            return;
        }
    }
}

} // namespace ps5
