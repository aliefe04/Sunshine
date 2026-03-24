/**
 * @file src/platform/windows/virtual_mic.cpp
 * @brief Windows WASAPI virtual microphone output implementation.
 *
 * Locates a virtual audio cable render device (VB-Cable "CABLE Input" or a
 * user-configured device name) and streams audio to it using WASAPI shared
 * mode. Handles sample rate and channel conversion using simple linear
 * interpolation.
 */

#define INITGUID
#define WIN32_LEAN_AND_MEAN

// standard includes
#include <algorithm>
#include <cstring>
#include <string>

// Windows includes
#include <windows.h>
#include <Audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <propsys.h>
#include <ks.h>
#include <ksmedia.h>

// local includes
#include "src/config.h"
#include "src/logging.h"
#include "virtual_mic.h"

// PKEY_Device_FriendlyName GUID
DEFINE_PROPERTYKEY(PKEY_VirtualMic_Device_FriendlyName,
  0xa45c254e, 0xdf1c, 0x4efd, 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0, 14);

namespace platf::virtual_mic {
  using namespace std::literals;

  static inline IMMDevice       *as_device(void *p)        { return static_cast<IMMDevice *>(p); }
  static inline IAudioClient    *as_audio_client(void *p)  { return static_cast<IAudioClient *>(p); }
  static inline IAudioRenderClient *as_render_client(void *p) { return static_cast<IAudioRenderClient *>(p); }

  // Helper to convert HRESULT to hex string for logging
  static std::string hr_to_hex(HRESULT hr) {
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%08X", static_cast<uint32_t>(hr));
    return std::string(buf);
  }

  virtual_mic_output_t::~virtual_mic_output_t() {
    active_ = false;

    if (render_client_)  { as_render_client(render_client_)->Release();  render_client_  = nullptr; }
    if (audio_client_)   { as_audio_client(audio_client_)->Stop();
                           as_audio_client(audio_client_)->Release();    audio_client_   = nullptr; }
    if (device_)         { as_device(device_)->Release();                device_         = nullptr; }

    delete[] resample_buffer_;
    resample_buffer_ = nullptr;

    BOOST_LOG(info) << "Virtual mic output destroyed";
  }

  void *virtual_mic_output_t::find_device(const std::string &name) {
    HRESULT hr;

    IMMDeviceEnumerator *enumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&enumerator));
    if (FAILED(hr)) {
      BOOST_LOG(error) << "CoCreateInstance(MMDeviceEnumerator) failed: " << hr_to_hex(hr);
      return nullptr;
    }

    // VB-Cable INPUT is a RENDER endpoint (we write audio into it)
    IMMDeviceCollection *collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    enumerator->Release();
    if (FAILED(hr)) {
      BOOST_LOG(error) << "EnumAudioEndpoints failed: " << hr_to_hex(hr);
      return nullptr;
    }

    UINT count = 0;
    collection->GetCount(&count);

    BOOST_LOG(info) << "Scanning " << count << " render devices for virtual mic...";

    IMMDevice *found = nullptr;
    for (UINT i = 0; i < count && found == nullptr; ++i) {
      IMMDevice *dev = nullptr;
      if (FAILED(collection->Item(i, &dev))) continue;

      IPropertyStore *props = nullptr;
      if (FAILED(dev->OpenPropertyStore(STGM_READ, &props))) { dev->Release(); continue; }

      PROPVARIANT pv;
      PropVariantInit(&pv);
      if (SUCCEEDED(props->GetValue(PKEY_VirtualMic_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) {
        char narrow[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
        std::string dev_name(narrow);

        BOOST_LOG(debug) << "  Render device: " << dev_name;

        bool match;
        if (name.empty()) {
          match = dev_name.find("CABLE Input") != std::string::npos ||
                  dev_name.find("VB-Audio")    != std::string::npos;
        } else {
          match = dev_name.find(name) != std::string::npos;
        }

        if (match) {
          BOOST_LOG(info) << "Found virtual mic device: " << dev_name;
          found = dev;
          dev = nullptr;
        }
      }

      PropVariantClear(&pv);
      props->Release();
      if (dev) dev->Release();
    }

    collection->Release();

    if (!found) {
      BOOST_LOG(error) << "No virtual audio cable found. "
                          "Install VB-Cable from https://vb-audio.com/Cable/ "
                          "or set mic_virtual_device in sunshine.conf";
    }

    return found;
  }

  int virtual_mic_output_t::init(const std::string &device_name,
                                  int channels, int sample_rate) {
    src_channels_   = channels;
    src_sample_rate_ = sample_rate;

    // CoInitializeEx is idempotent — call on every thread that uses WASAPI
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
      BOOST_LOG(error) << "CoInitializeEx failed: " << hr_to_hex(hr);
      return -1;
    }

    device_ = find_device(device_name);
    if (!device_) return -1;

    // Activate audio client
    IAudioClient *client = nullptr;
    hr = as_device(device_)->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                       nullptr, reinterpret_cast<void **>(&client));
    if (FAILED(hr)) {
      BOOST_LOG(error) << "IAudioClient::Activate failed: " << hr_to_hex(hr);
      return -1;
    }
    audio_client_ = client;

    // Use the device's native mix format — avoids any format-mismatch crash.
    WAVEFORMATEX *mix_fmt = nullptr;
    hr = client->GetMixFormat(&mix_fmt);
    if (FAILED(hr) || !mix_fmt) {
      BOOST_LOG(error) << "IAudioClient::GetMixFormat failed: " << hr_to_hex(hr);
      return -1;
    }

    dev_channels_    = static_cast<int>(mix_fmt->nChannels);
    dev_sample_rate_ = static_cast<int>(mix_fmt->nSamplesPerSec);
    dev_block_align_ = static_cast<int>(mix_fmt->nBlockAlign);

    // Determine whether the device format is float32 or int16
    if (mix_fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
      dev_is_float_ = true;
    } else if (mix_fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
      auto *ext = reinterpret_cast<WAVEFORMATEXTENSIBLE *>(mix_fmt);
      dev_is_float_ = (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    } else {
      dev_is_float_ = false;  // assume PCM int16
    }

    BOOST_LOG(info) << "Virtual mic device format: "
                    << dev_channels_ << "ch, "
                    << dev_sample_rate_ << "Hz, "
                    << mix_fmt->wBitsPerSample << "-bit "
                    << (dev_is_float_ ? "float" : "int");

    BOOST_LOG(info) << "Source format: "
                    << src_channels_ << "ch, "
                    << src_sample_rate_ << "Hz";

    // Calculate resampling ratio
    sample_ratio_ = static_cast<double>(dev_sample_rate_) / static_cast<double>(src_sample_rate_);

    // Allocate resample buffer (worst case: 4x expansion for sample rate + channels)
    resample_buffer_size_ = 8192 * static_cast<size_t>(dev_channels_) * 4;  // 8K frames, 4 bytes/sample
    resample_buffer_ = new float[resample_buffer_size_ / sizeof(float)];
    if (!resample_buffer_) {
      BOOST_LOG(error) << "Failed to allocate resample buffer";
      CoTaskMemFree(mix_fmt);
      return -1;
    }

    constexpr REFERENCE_TIME buf_duration = 4000000;  // 400 ms in 100-ns units (larger buffer)
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, buf_duration, 0, mix_fmt, nullptr);
    CoTaskMemFree(mix_fmt);
    if (FAILED(hr)) {
      BOOST_LOG(error) << "IAudioClient::Initialize failed: " << hr_to_hex(hr);
      return -1;
    }

    hr = client->GetBufferSize(&buffer_frames_);
    if (FAILED(hr)) {
      BOOST_LOG(error) << "IAudioClient::GetBufferSize failed: " << hr_to_hex(hr);
      return -1;
    }
    BOOST_LOG(info) << "WASAPI buffer: " << buffer_frames_ << " frames ("
                    << (buffer_frames_ * 1000.0 / dev_sample_rate_) << " ms)";

    IAudioRenderClient *render = nullptr;
    hr = client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void **>(&render));
    if (FAILED(hr)) {
      BOOST_LOG(error) << "GetService(IAudioRenderClient) failed: " << hr_to_hex(hr);
      return -1;
    }
    render_client_ = render;

    hr = client->Start();
    if (FAILED(hr)) {
      BOOST_LOG(error) << "IAudioClient::Start failed: " << hr_to_hex(hr);
      return -1;
    }

    active_ = true;
    BOOST_LOG(info) << "Virtual mic WASAPI stream started successfully";
    return 0;
  }

  // Simple linear interpolation resampling
  static void resample_mono_to_stereo_float(const opus_int16 *src, float *dst,
                                            int src_frames, int dst_frames,
                                            double ratio) {
    for (int i = 0; i < dst_frames; i++) {
      double src_pos = static_cast<double>(i) / ratio;
      int src_idx = static_cast<int>(src_pos);
      double frac = src_pos - src_idx;

      float s0 = (src_idx < src_frames) ? static_cast<float>(src[src_idx]) / 32768.0f : 0.0f;
      float s1 = (src_idx + 1 < src_frames) ? static_cast<float>(src[src_idx + 1]) / 32768.0f : s0;

      // Linear interpolation
      float sample = s0 * (1.0f - static_cast<float>(frac)) + s1 * static_cast<float>(frac);

      // Mono to stereo (duplicate to both channels)
      dst[i * 2] = sample;
      dst[i * 2 + 1] = sample;
    }
  }

  // Direct copy with format conversion (no resampling)
  static void convert_mono_to_stereo_float(const opus_int16 *src, float *dst, int frames) {
    for (int i = 0; i < frames; i++) {
      float sample = static_cast<float>(src[i]) / 32768.0f;
      dst[i * 2] = sample;
      dst[i * 2 + 1] = sample;
    }
  }

  int virtual_mic_output_t::write_pcm(const opus_int16 *data, int frames) {
    if (!active_ || !render_client_ || !audio_client_) {
      BOOST_LOG(warning) << "write_pcm called but not active";
      return -1;
    }

    IAudioClient       *client = as_audio_client(audio_client_);
    IAudioRenderClient *render = as_render_client(render_client_);

    UINT32 padding = 0;
    HRESULT hr = client->GetCurrentPadding(&padding);
    if (FAILED(hr)) {
      BOOST_LOG(warning) << "GetCurrentPadding failed: " << hr_to_hex(hr);
      return -1;
    }

    UINT32 available = buffer_frames_ - padding;

    // If buffer is nearly full, log warning but don't fail
    if (available < static_cast<UINT32>(frames / 2)) {
      BOOST_LOG(verbose) << "WASAPI buffer nearly full (" << available << " frames available, "
                         << frames << " frames to write)";
    }

    // If no space available, drop frame (better than blocking)
    if (available == 0) {
      BOOST_LOG(verbose) << "WASAPI buffer full, dropping frame";
      return 0;
    }

    // Calculate how many output frames we'll produce
    int output_frames;
    if (sample_ratio_ != 1.0) {
      output_frames = static_cast<int>(frames * sample_ratio_);
    } else {
      output_frames = frames;
    }

    UINT32 write_frames = std::min(static_cast<UINT32>(output_frames), available);

    BYTE *buf = nullptr;
    hr = render->GetBuffer(write_frames, &buf);
    if (FAILED(hr)) {
      BOOST_LOG(warning) << "IAudioRenderClient::GetBuffer failed: " << hr_to_hex(hr);
      return -1;
    }

    if (!buf) {
      BOOST_LOG(warning) << "GetBuffer returned nullptr";
      render->ReleaseBuffer(0, 0);
      return -1;
    }

    // Convert and resample audio
    if (dev_is_float_) {
      auto *fbuf = reinterpret_cast<float *>(buf);

      if (sample_ratio_ != 1.0) {
        // Resample from src_sample_rate to dev_sample_rate
        resample_mono_to_stereo_float(data, fbuf, frames, static_cast<int>(write_frames), sample_ratio_);
      } else {
        // Direct conversion (no resampling needed)
        convert_mono_to_stereo_float(data, fbuf, static_cast<int>(write_frames));
      }
    } else {
      // Integer output (rare, but handle it)
      auto *ibuf = reinterpret_cast<opus_int16 *>(buf);
      for (UINT32 i = 0; i < write_frames; i++) {
        int src_idx = (sample_ratio_ != 1.0)
                      ? static_cast<int>(i / sample_ratio_)
                      : static_cast<int>(i);
        if (src_idx >= frames) src_idx = frames - 1;

        opus_int16 sample = data[src_idx];
        // Mono to stereo
        ibuf[i * dev_channels_] = sample;
        if (dev_channels_ > 1) {
          ibuf[i * dev_channels_ + 1] = sample;
        }
      }
    }

    render->ReleaseBuffer(write_frames, 0);
    return 0;
  }

}  // namespace platf::virtual_mic