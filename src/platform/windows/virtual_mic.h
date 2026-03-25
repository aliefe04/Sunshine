/**
 * @file src/platform/windows/virtual_mic.h
 * @brief Windows WASAPI-based virtual microphone output.
 *
 * Uses Steam Streaming Microphone for audio output. Steam must be installed
 * on the host for microphone passthrough to work.
 */

#pragma once

// standard includes
#include <cstdint>
#include <string>

// lib includes
#include <opus/opus.h>

namespace platf::virtual_mic {

  /**
   * @brief Check if Steam Streaming Microphone is installed.
   * @return true if Steam Streaming Microphone is found.
   */
  bool is_steam_mic_available();

  /**
   * @brief Writes 16-bit PCM audio to a WASAPI render device (Steam Streaming Microphone).
   *
   * Lifecycle: construct → init() → write_pcm() repeatedly → destroy.
   * The class is NOT thread-safe; external locking is required if called from
   * multiple threads.
   */
  class virtual_mic_output_t {
  public:
    virtual_mic_output_t() = default;
    ~virtual_mic_output_t();

    /**
     * @brief Open the Steam Streaming Microphone and prepare the WASAPI client.
     * @param channels     Number of channels (1 = mono, 2 = stereo).
     * @param sample_rate  Sample rate in Hz (e.g. 48000).
     * @return 0 on success, -1 on failure.
     */
    int init(int channels, int sample_rate);

    /**
     * @brief Write a block of 16-bit signed PCM samples to the device.
     * @param data    Pointer to interleaved 16-bit signed samples.
     * @param frames  Number of audio frames (samples per channel).
     * @return 0 on success, -1 on failure.
     */
    int write_pcm(const opus_int16 *data, int frames);

    bool is_active() const {
      return active_;
    }

  private:
    /** @brief Find Steam Streaming Microphone device. */
    void *find_steam_device();

    // Raw COM interface pointers — managed manually to avoid unique_ptr<COM> pitfalls
    void *device_ = nullptr;  // IMMDevice*
    void *audio_client_ = nullptr;  // IAudioClient*
    void *render_client_ = nullptr;  // IAudioRenderClient*

    bool active_ = false;

    // Source (Opus decoder) format
    int src_channels_ = 1;
    int src_sample_rate_ = 48000;

    // Device (WASAPI mix) format — may differ from source
    int dev_channels_ = 1;
    int dev_sample_rate_ = 48000;
    int dev_block_align_ = 4;  ///< bytes per frame on the device
    bool dev_is_float_ = true;  ///< true = IEEE float32, false = int16

    uint32_t buffer_frames_ = 0;  ///< Total WASAPI shared-mode buffer size in frames

    // Resampling
    double sample_ratio_ = 1.0;  ///< dev_sample_rate / src_sample_rate
    float *resample_buffer_ = nullptr;
    size_t resample_buffer_size_ = 0;
  };

}  // namespace platf::virtual_mic
