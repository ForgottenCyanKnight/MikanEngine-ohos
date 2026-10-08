#include "audio_manager.h"

#include <ohaudio/native_audiorenderer.h>
#include <ohaudio/native_audiostreambuilder.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

extern "C" int OHOS_ReadRawFile(const char* path, void** data, size_t* size);
extern "C" void OHOS_FreeRawFile(void* data);

namespace {

constexpr int32_t kPcmFormat = 1;
constexpr int32_t kFloatFormat = 3;
constexpr int32_t kExtensibleFormat = 0xfffe;

uint16_t ReadU16(const uint8_t* bytes)
{
    return static_cast<uint16_t>(bytes[0]) |
        static_cast<uint16_t>(static_cast<uint16_t>(bytes[1]) << 8);
}

uint32_t ReadU32(const uint8_t* bytes)
{
    return static_cast<uint32_t>(bytes[0]) |
        (static_cast<uint32_t>(bytes[1]) << 8) |
        (static_cast<uint32_t>(bytes[2]) << 16) |
        (static_cast<uint32_t>(bytes[3]) << 24);
}

float DecodeSample(const uint8_t* sample, uint16_t bitsPerSample, int32_t format)
{
    if (format == kFloatFormat && bitsPerSample == 32) {
        const uint32_t bits = ReadU32(sample);
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return std::isfinite(value) ? std::clamp(value, -1.0f, 1.0f) : 0.0f;
    }

    switch (bitsPerSample) {
    case 8:
        return (static_cast<int32_t>(sample[0]) - 128) / 128.0f;
    case 16: {
        int32_t value = ReadU16(sample);
        if ((value & 0x8000) != 0) {
            value -= 0x10000;
        }
        return value / 32768.0f;
    }
    case 24: {
        int32_t value = static_cast<int32_t>(sample[0]) |
            (static_cast<int32_t>(sample[1]) << 8) |
            (static_cast<int32_t>(sample[2]) << 16);
        if ((value & 0x800000) != 0) {
            value |= static_cast<int32_t>(0xff000000u);
        }
        return value / 8388608.0f;
    }
    case 32: {
        int64_t value = ReadU32(sample);
        if ((value & 0x80000000ll) != 0) {
            value -= 0x100000000ll;
        }
        return static_cast<float>(value / 2147483648.0);
    }
    default:
        return 0.0f;
    }
}

bool DecodeWav(const uint8_t* bytes, std::size_t byteCount, std::vector<float>& monoSamples,
    uint32_t& sampleRate)
{
    if (bytes == nullptr || byteCount < 12 || std::memcmp(bytes, "RIFF", 4) != 0 ||
        std::memcmp(bytes + 8, "WAVE", 4) != 0) {
        return false;
    }

    int32_t format = 0;
    uint16_t channels = 0;
    uint16_t bitsPerSample = 0;
    uint16_t blockAlign = 0;
    const uint8_t* sampleData = nullptr;
    std::size_t sampleDataSize = 0;

    for (std::size_t offset = 12; offset <= byteCount && byteCount - offset >= 8;) {
        const uint8_t* chunk = bytes + offset;
        const uint32_t chunkSize = ReadU32(chunk + 4);
        const std::size_t payloadOffset = offset + 8;
        if (chunkSize > byteCount - payloadOffset) {
            return false;
        }

        if (std::memcmp(chunk, "fmt ", 4) == 0) {
            if (chunkSize < 16) {
                return false;
            }
            const uint8_t* fmt = bytes + payloadOffset;
            format = ReadU16(fmt);
            channels = ReadU16(fmt + 2);
            sampleRate = ReadU32(fmt + 4);
            blockAlign = ReadU16(fmt + 12);
            bitsPerSample = ReadU16(fmt + 14);
            if (format == kExtensibleFormat && chunkSize >= 40) {
                format = ReadU16(fmt + 24);
            }
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            sampleData = bytes + payloadOffset;
            sampleDataSize = chunkSize;
        }

        const std::size_t next = payloadOffset + chunkSize;
        offset = next + ((chunkSize & 1u) != 0u && next < byteCount ? 1u : 0u);
    }

    if (sampleData == nullptr || sampleDataSize == 0 || sampleRate == 0 || channels == 0 ||
        channels > 8 || blockAlign == 0 ||
        (format != kPcmFormat && format != kFloatFormat) ||
        (format == kFloatFormat && bitsPerSample != 32) ||
        (format == kPcmFormat && bitsPerSample != 8 && bitsPerSample != 16 &&
            bitsPerSample != 24 && bitsPerSample != 32)) {
        return false;
    }

    const std::size_t bytesPerSample = bitsPerSample / 8;
    if (bytesPerSample == 0 || blockAlign < bytesPerSample * channels ||
        sampleDataSize % blockAlign != 0) {
        return false;
    }
    const std::size_t frameCount = sampleDataSize / blockAlign;
    if (frameCount == 0) {
        return false;
    }

    monoSamples.resize(frameCount);
    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        const uint8_t* frameData = sampleData + frame * blockAlign;
        float mixed = 0.0f;
        for (uint16_t channel = 0; channel < channels; ++channel) {
            mixed += DecodeSample(frameData + channel * bytesPerSample, bitsPerSample, format);
        }
        monoSamples[frame] = std::clamp(mixed / channels, -1.0f, 1.0f);
    }
    return true;
}

bool IsValidGain(float gain)
{
    return std::isfinite(gain) && gain >= 0.0f && gain <= 1.0f;
}

} // namespace

AudioManager& AudioManager::GetInstance()
{
    static AudioManager instance;
    return instance;
}

AudioManager::~AudioManager()
{
    Shutdown();
}

bool AudioManager::Initialize()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (renderer_ != nullptr) {
            return true;
        }
        shuttingDown_ = false;
    }

    OH_AudioStreamBuilder* builder = nullptr;
    if (OH_AudioStreamBuilder_Create(&builder, AUDIOSTREAM_TYPE_RENDERER) != AUDIOSTREAM_SUCCESS ||
        builder == nullptr) {
        return false;
    }

    OH_AudioRenderer* renderer = nullptr;
    const bool configured =
        OH_AudioStreamBuilder_SetSamplingRate(builder, kOutputSampleRate) == AUDIOSTREAM_SUCCESS &&
        OH_AudioStreamBuilder_SetChannelCount(builder, kOutputChannels) == AUDIOSTREAM_SUCCESS &&
        OH_AudioStreamBuilder_SetSampleFormat(builder, AUDIOSTREAM_SAMPLE_S16LE) == AUDIOSTREAM_SUCCESS &&
        OH_AudioStreamBuilder_SetEncodingType(builder, AUDIOSTREAM_ENCODING_TYPE_RAW) == AUDIOSTREAM_SUCCESS &&
        OH_AudioStreamBuilder_SetRendererInfo(builder, AUDIOSTREAM_USAGE_GAME) == AUDIOSTREAM_SUCCESS &&
        OH_AudioStreamBuilder_SetRendererWriteDataCallback(builder, OnWriteData, this) == AUDIOSTREAM_SUCCESS &&
        OH_AudioStreamBuilder_GenerateRenderer(builder, &renderer) == AUDIOSTREAM_SUCCESS &&
        renderer != nullptr;
    OH_AudioStreamBuilder_Destroy(builder);
    if (!configured || renderer == nullptr) {
        if (renderer != nullptr) {
            OH_AudioRenderer_Release(renderer);
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        renderer_ = renderer;
    }
    if (OH_AudioRenderer_Start(renderer) != AUDIOSTREAM_SUCCESS) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            renderer_ = nullptr;
            shuttingDown_ = true;
        }
        OH_AudioRenderer_Release(renderer);
        return false;
    }
    return true;
}

void AudioManager::Shutdown()
{
    OH_AudioRenderer* renderer = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shuttingDown_ = true;
        renderer = renderer_;
    }

    if (renderer != nullptr) {
        OH_AudioRenderer_Stop(renderer);
        OH_AudioRenderer_Release(renderer);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    renderer_ = nullptr;
    sounds_.clear();
}

bool AudioManager::LoadAudio(const std::string& name, const std::string& rawFilePath)
{
    if (name.empty() || rawFilePath.empty()) {
        return false;
    }

    void* rawBytes = nullptr;
    std::size_t rawSize = 0;
    if (!OHOS_ReadRawFile(rawFilePath.c_str(), &rawBytes, &rawSize) || rawBytes == nullptr || rawSize == 0) {
        return false;
    }

    Sound decoded;
    const bool validWav = DecodeWav(static_cast<const uint8_t*>(rawBytes), rawSize,
        decoded.monoSamples, decoded.sampleRate);
    OHOS_FreeRawFile(rawBytes);
    if (!validWav) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (shuttingDown_) {
        return false;
    }
    sounds_[name] = std::move(decoded);
    return true;
}

bool AudioManager::PlayAudio(const std::string& name, float volume, bool loop)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (renderer_ == nullptr || shuttingDown_) {
        return false;
    }
    const auto found = sounds_.find(name);
    if (found == sounds_.end() || found->second.monoSamples.empty()) {
        return false;
    }

    Sound& sound = found->second;
    std::size_t selected = sound.nextVoice;
    for (std::size_t offset = 0; offset < sound.voices.size(); ++offset) {
        const std::size_t candidate = (sound.nextVoice + offset) % sound.voices.size();
        if (!sound.voices[candidate].playing) {
            selected = candidate;
            break;
        }
    }

    Voice& voice = sound.voices[selected];
    voice.framePosition = 0.0;
    voice.gain = IsValidGain(volume) ? volume : sound.defaultGain;
    voice.looping = loop;
    voice.playing = true;
    sound.nextVoice = (selected + 1) % sound.voices.size();
    return true;
}

void AudioManager::StopAudio(const std::string& name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = sounds_.find(name);
    if (found == sounds_.end()) {
        return;
    }
    for (Voice& voice : found->second.voices) {
        voice.playing = false;
    }
}

void AudioManager::StopAll()
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& pair : sounds_) {
        for (Voice& voice : pair.second.voices) {
            voice.playing = false;
        }
    }
}

bool AudioManager::SetVolume(const std::string& name, float volume)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = sounds_.find(name);
    if (found == sounds_.end()) {
        return false;
    }
    const float gain = IsValidGain(volume) ? volume : 1.0f;
    found->second.defaultGain = gain;
    for (Voice& voice : found->second.voices) {
        if (voice.playing) {
            voice.gain = gain;
        }
    }
    return true;
}

bool AudioManager::IsPlaying(const std::string& name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = sounds_.find(name);
    if (found == sounds_.end()) {
        return false;
    }
    return std::any_of(found->second.voices.begin(), found->second.voices.end(),
        [](const Voice& voice) { return voice.playing; });
}

void AudioManager::Pause()
{
    OH_AudioRenderer* renderer = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!shuttingDown_) {
            renderer = renderer_;
        }
    }
    if (renderer != nullptr) {
        OH_AudioRenderer_Pause(renderer);
    }
}

void AudioManager::Resume()
{
    OH_AudioRenderer* renderer = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!shuttingDown_) {
            renderer = renderer_;
        }
    }
    if (renderer != nullptr) {
        OH_AudioRenderer_Start(renderer);
    }
}

OH_AudioData_Callback_Result AudioManager::OnWriteData(
    OH_AudioRenderer* /*renderer*/, void* userData, void* audioData, int32_t audioDataSize)
{
    if (userData == nullptr || audioData == nullptr || audioDataSize <= 0) {
        return AUDIO_DATA_CALLBACK_RESULT_INVALID;
    }

    const std::size_t bytes = static_cast<std::size_t>(audioDataSize);
    std::memset(audioData, 0, bytes);
    constexpr std::size_t bytesPerFrame = sizeof(int16_t) * kOutputChannels;
    const std::size_t frameCount = bytes / bytesPerFrame;
    const bool hasAudio = static_cast<AudioManager*>(userData)->Mix(
        static_cast<int16_t*>(audioData), frameCount);
    return hasAudio ? AUDIO_DATA_CALLBACK_RESULT_VALID : AUDIO_DATA_CALLBACK_RESULT_INVALID;
}

bool AudioManager::Mix(int16_t* output, std::size_t frameCount)
{
    if (output == nullptr || frameCount == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (shuttingDown_) {
        return false;
    }

    bool hasAudio = false;
    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        float mixed = 0.0f;
        for (auto& pair : sounds_) {
            Sound& sound = pair.second;
            if (sound.monoSamples.empty() || sound.sampleRate == 0) {
                continue;
            }
            for (Voice& voice : sound.voices) {
                if (!voice.playing) {
                    continue;
                }
                if (voice.framePosition >= sound.monoSamples.size()) {
                    if (voice.looping) {
                        voice.framePosition = std::fmod(voice.framePosition,
                            static_cast<double>(sound.monoSamples.size()));
                    } else {
                        voice.playing = false;
                        continue;
                    }
                }

                const std::size_t sampleIndex = static_cast<std::size_t>(voice.framePosition);
                const std::size_t nextIndex = sampleIndex + 1 < sound.monoSamples.size()
                    ? sampleIndex + 1
                    : (voice.looping ? 0 : sampleIndex);
                const float fraction = static_cast<float>(voice.framePosition - sampleIndex);
                const float sample = sound.monoSamples[sampleIndex] * (1.0f - fraction) +
                    sound.monoSamples[nextIndex] * fraction;
                mixed += sample * voice.gain;
                voice.framePosition += static_cast<double>(sound.sampleRate) / kOutputSampleRate;
                hasAudio = true;
            }
        }

        const float clipped = std::clamp(mixed, -1.0f, 1.0f);
        const int16_t sample = static_cast<int16_t>(std::lrint(clipped * 32767.0f));
        output[frame * kOutputChannels] = sample;
        output[frame * kOutputChannels + 1] = sample;
    }
    return hasAudio;
}
