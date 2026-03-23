/**
 * @file src/platform/windows/virtual_mic.h
 * @brief Declarations for Windows virtual microphone output.
 */
#pragma once

// standard includes
#include <memory>
#include <string>
#include <vector>

// local includes
#include "mic_stream.h"

namespace platf::virtual_mic {
  /**
   * @brief Windows virtual microphone output using WASAPI.
   * Outputs decoded mic audio to a virtual audio device (e.g., VB-Audio Virtual Cable).
   */
  class virtual_mic_output_t {
  public:
    virtual_mic_output_t() = default;
    ~virtual_mic_output_t();

    /**
     * @brief Initialize the virtual mic output device.
     * @param device_name The name of the virtual device to use (e.g., "CABLE Input").
     *                    If empty, will auto-detect VB-Audio Virtual Cable.
     * @param channels Number of audio channels (1 = mono, 2 = stereo).
     * @param sample_rate Sample rate in Hz (e.g., 48000).
     * @return 0 on success, non-zero on failure.
     */
    int init(const std::string &device_name, int channels, int sample_rate);

    /**
     * @brief Write PCM audio data to the virtual device.
     * @param pcm_data Pointer to PCM data (16-bit signed, interleaved).
     * @param samples Number of samples per channel.
     * @return 0 on success, non-zero on failure.
     */
    int write_pcm(const float *pcm_data, int samples);

    /**
     * @brief Check if the virtual mic is active.
     */
    bool is_active() const { return active_; }

  private:
    /**
     * @brief Find the VB-Cable input device by name.
     * @param device_name The device name to search for.
     * @return Device pointer, or nullptr if not found.
     */
    void *find_device(const std::string &device_name);

    bool active_ = false;
    int channels_ = 1;
    int sample_rate_ = 48000;

    // WASAPI objects (using void* to avoid Windows header pollution)
    void *device_ = nullptr;           // IMMDevice
    void *audio_client_ = nullptr;     // IAudioClient
    void *render_client_ = nullptr;    // IAudioRenderClient
    void *event_handle_ = nullptr;     // HANDLE

    // Buffer for format conversion
    std::vector<int16_t> int16_buffer_;
  };

}  // namespace platf::virtual_mic
