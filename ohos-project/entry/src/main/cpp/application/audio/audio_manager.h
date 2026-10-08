#pragma once

#include <ohaudio/native_audiostream_base.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// Small OHAudio-backed counterpart to Mikan's AudioManager. Clips are decoded
// from OHOS rawfiles and each named sound can have up to four overlapping voices.
class AudioManager final {
public:
    static AudioManager& GetInstance();

    bool Initialize();
    void Shutdown();

    bool LoadAudio(const std::string& name, const std::string& rawFilePath);
    bool PlayAudio(const std::string& name, float volume = 1.0f, bool loop = false);
    void StopAudio(const std::string& name);
    void StopAll();
    bool SetVolume(const std::string& name, float volume);
    bool IsPlaying(const std::string& name);

    void Pause();
    void Resume();

private:
    struct Voice {
        double framePosition = 0.0;
        float gain = 1.0f;
        bool looping = false;
        bool playing = false;
    };

    struct Sound {
        std::vector<float> monoSamples;
        uint32_t sampleRate = 0;
        std::array<Voice, 4> voices{};
        std::size_t nextVoice = 0;
        float defaultGain = 1.0f;
    };

    AudioManager() = default;
    ~AudioManager();
    AudioManager(const AudioManager&) = delete;
    AudioManager& operator=(const AudioManager&) = delete;

    static OH_AudioData_Callback_Result OnWriteData(
        OH_AudioRenderer* renderer, void* userData, void* audioData, int32_t audioDataSize);
    bool Mix(int16_t* output, std::size_t frameCount);

    static constexpr int32_t kOutputSampleRate = 48000;
    static constexpr int32_t kOutputChannels = 2;

    std::mutex mutex_;
    std::map<std::string, Sound> sounds_;
    OH_AudioRenderer* renderer_ = nullptr;
    bool shuttingDown_ = false;
};
