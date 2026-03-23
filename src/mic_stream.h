/**
 * @file src/mic_stream.h
 * @brief Declarations for microphone passthrough stream handling.
 */
#pragma once

// standard includes
#include <memory>
#include <mutex>
#include <vector>

// lib includes
#include <opus/opus.h>

// local includes
#include "thread_safe.h"
#include "utility.h"

// Platform-specific virtual mic output
#ifdef _WIN32
#include "platform/windows/virtual_mic.h"
#endif

namespace mic_stream {
  constexpr auto SAMPLE_RATE = 48000;
  constexpr auto FRAME_DURATION_MS = 20;
  constexpr auto SAMPLES_PER_FRAME = SAMPLE_RATE * FRAME_DURATION_MS / 1000;  // 960 samples
  constexpr auto BYTES_PER_SAMPLE = 2;  // 16-bit signed
  constexpr auto FRAME_SIZE_BYTES = SAMPLES_PER_FRAME * BYTES_PER_SAMPLE;  // 1920 bytes

  /**
   * @brief Configuration for a microphone stream.
   */
  struct config_t {
    uint8_t audio_input_id = 0;
    uint8_t channels = 1;
    uint32_t sample_rate = SAMPLE_RATE;
    uint32_t bitrate = 64000;
  };

  /**
   * @brief State for a single microphone stream.
   */
  class mic_stream_t {
  public:
    mic_stream_t(const config_t &config);
    ~mic_stream_t();

    /**
     * @brief Initialize the microphone stream (create Opus decoder and virtual output).
     * @return 0 on success, non-zero on failure.
     */
    int start();

    /**
     * @brief Stop the microphone stream (destroy Opus decoder and virtual output).
     */
    void stop();

    /**
     * @brief Process incoming Opus-encoded mic data.
     * @param data Pointer to Opus-encoded data.
     * @param size Size of the Opus data in bytes.
     * @return 0 on success, non-zero on failure.
     */
    int process_opus_data(const uint8_t *data, size_t size);

    /**
     * @brief Check if the stream is active.
     */
    bool is_active() const { return active_; }

    /**
     * @brief Get the configuration.
     */
    const config_t &config() const { return config_; }

  private:
    config_t config_;
    OpusDecoder *opus_decoder_ = nullptr;
    bool active_ = false;
    std::vector<float> pcm_buffer_;
    std::mutex mutex_;

    // Platform-specific virtual mic output
#ifdef _WIN32
    std::unique_ptr<platf::virtual_mic::virtual_mic_output_t> virtual_output_;
#endif
  };

  /**
   * @brief Manager for all microphone streams.
   */
  class mic_stream_manager_t {
  public:
    mic_stream_manager_t() = default;
    ~mic_stream_manager_t() = default;

    /**
     * @brief Start a new microphone stream.
     * @param config Configuration for the stream.
     * @return Pointer to the stream, or nullptr on failure.
     */
    std::shared_ptr<mic_stream_t> start_stream(const config_t &config);

    /**
     * @brief Stop a microphone stream.
     * @param audio_input_id The audio input ID of the stream to stop.
     */
    void stop_stream(uint8_t audio_input_id);

    /**
     * @brief Get a stream by audio input ID.
     * @param audio_input_id The audio input ID.
     * @return Pointer to the stream, or nullptr if not found.
     */
    std::shared_ptr<mic_stream_t> get_stream(uint8_t audio_input_id);

    /**
     * @brief Stop all streams.
     */
    void stop_all();

  private:
    std::unordered_map<uint8_t, std::shared_ptr<mic_stream_t>> streams_;
    std::mutex mutex_;
  };

  // Global mic stream manager
  extern mic_stream_manager_t g_mic_stream_manager;

}  // namespace mic_stream
