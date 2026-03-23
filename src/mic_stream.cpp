/**
 * @file src/mic_stream.cpp
 * @brief Definitions for microphone passthrough stream handling.
 */

// standard includes
#include <cstring>

// local includes
#include "config.h"
#include "logging.h"
#include "mic_stream.h"

namespace mic_stream {
  using namespace std::literals;

  mic_stream_manager_t g_mic_stream_manager;

  mic_stream_t::mic_stream_t(const config_t &config) :
      config_(config) {
    pcm_buffer_.resize(SAMPLES_PER_FRAME * config_.channels);
  }

  mic_stream_t::~mic_stream_t() {
    stop();
  }

  int mic_stream_t::start() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (active_) {
      BOOST_LOG(warning) << "Mic stream already active"sv;
      return 0;
    }

    // Create Opus decoder
    int error = 0;
    opus_decoder_ = opus_decoder_create(config_.sample_rate, config_.channels, &error);
    if (error != OPUS_OK) {
      BOOST_LOG(error) << "Failed to create Opus decoder: "sv << opus_strerror(error);
      return -1;
    }

#ifdef _WIN32
    // Initialize virtual mic output (WASAPI)
    virtual_output_ = std::make_unique<platf::virtual_mic::virtual_mic_output_t>();
    std::string device_name = config::audio.mic_virtual_device;
    if (virtual_output_->init(device_name, config_.channels, config_.sample_rate) != 0) {
      BOOST_LOG(error) << "Failed to initialize virtual mic output"sv;
      opus_decoder_destroy(opus_decoder_);
      opus_decoder_ = nullptr;
      return -1;
    }
#endif

    active_ = true;
    BOOST_LOG(info) << "Mic stream started: channels=" << (int) config_.channels
                    << ", sample_rate=" << config_.sample_rate
                    << ", bitrate=" << config_.bitrate;

    return 0;
  }

  void mic_stream_t::stop() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!active_) {
      return;
    }

#ifdef _WIN32
    // Clean up virtual mic output
    virtual_output_.reset();
#endif

    if (opus_decoder_) {
      opus_decoder_destroy(opus_decoder_);
      opus_decoder_ = nullptr;
    }

    active_ = false;
    BOOST_LOG(info) << "Mic stream stopped"sv;
  }

  int mic_stream_t::process_opus_data(const uint8_t *data, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!active_ || !opus_decoder_) {
      BOOST_LOG(verbose) << "Mic stream not active, ignoring data"sv;
      return 0;
    }

    // Decode Opus to PCM
    int samples = opus_decode(opus_decoder_, data, size,
                              pcm_buffer_.data(), SAMPLES_PER_FRAME,
                              0 /* decode_fec */);

    if (samples < 0) {
      BOOST_LOG(error) << "Opus decode failed: "sv << opus_strerror(samples);
      return -1;
    }

    BOOST_LOG(verbose) << "Decoded "sv << samples << " samples from "sv << size << " bytes Opus"sv;

#ifdef _WIN32
    // Output PCM to virtual audio device
    if (virtual_output_ && virtual_output_->is_active()) {
      if (virtual_output_->write_pcm(pcm_buffer_.data(), samples) != 0) {
        BOOST_LOG(warning) << "Failed to write PCM to virtual mic output"sv;
      }
    }
#else
    // TODO: Implement for other platforms (macOS, Linux)
    BOOST_LOG(verbose) << "Mic output not implemented for this platform"sv;
#endif

    return 0;
  }

  // mic_stream_manager_t implementation

  std::shared_ptr<mic_stream_t> mic_stream_manager_t::start_stream(const config_t &config) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check if stream already exists
    auto it = streams_.find(config.audio_input_id);
    if (it != streams_.end()) {
      BOOST_LOG(warning) << "Mic stream " << (int) config.audio_input_id << " already exists"sv;
      return it->second;
    }

    // Create new stream
    auto stream = std::make_shared<mic_stream_t>(config);
    if (stream->start() != 0) {
      return nullptr;
    }

    streams_[config.audio_input_id] = stream;
    return stream;
  }

  void mic_stream_manager_t::stop_stream(uint8_t audio_input_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = streams_.find(audio_input_id);
    if (it != streams_.end()) {
      it->second->stop();
      streams_.erase(it);
    }
  }

  std::shared_ptr<mic_stream_t> mic_stream_manager_t::get_stream(uint8_t audio_input_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = streams_.find(audio_input_id);
    if (it != streams_.end()) {
      return it->second;
    }
    return nullptr;
  }

  void mic_stream_manager_t::stop_all() {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto &pair : streams_) {
      pair.second->stop();
    }
    streams_.clear();
  }

}  // namespace mic_stream