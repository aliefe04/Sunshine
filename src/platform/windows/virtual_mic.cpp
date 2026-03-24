/**
 * @file src/platform/windows/virtual_mic.cpp
 * @brief Windows WASAPI virtual microphone output implementation.
 *
 * Locates a virtual audio cable render device (VB-Cable "CABLE Input" or a
 * user-configured device name) and streams audio to it using WASAPI shared
 * mode.  The device's native mix format is used (typically float32) so the
 * driver performs no extra conversion.
 */

#define INITGUID

// standard includes
#include <algorithm>
#include <cstring>
#include <string>

// platform includes
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

  virtual_mic_output_t::~virtual_mic_output_t() {
    active_ = false;
    if (render_client_)  { as_render_client(render_client_)->Release();  render_client_  = nullptr; }
    if (audio_client_)   { as_audio_client(audio_client_)->Stop();
                           as_audio_client(audio_client_)->Release();    audio_client_   = nullptr; }
    if (device_)         { as_device(device_)->Release();                device_         = nullptr; }
  }

  void *virtual_mic_output_t::find_device(const std::string &name) {
    HRESULT hr;

    IMMDeviceEnumerator *enumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&enumerator));
    if (FAILED(hr)) {
      BOOST_LOG(error) << "CoCreateInstance(MMDeviceEnumerator) failed: 0x" << std::hex << hr;
      return nullptr;
    }

    // VB-Cable INPUT is a RENDER endpoint (we write audio into it)
    IMMDeviceCollection *collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    enumerator->Release();
    if (FAILED(hr)) {
      BOOST_LOG(error) << "EnumAudioEndpoints failed: 0x" << std::hex << hr;
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
          BOOST_LOG(info) << "Selected virtual mic device: " << dev_name;
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
      BOOST_LOG(error) << "No virtual audio cable found."
                          " Ensure VB-Cable is installed (https://vb-audio.com/Cable/)"
                          " or set mic_virtual_device in sunshine.conf";
    }

    return found;
  }

  int virtual_mic_output_t::init(const std::string &device_name,
                                  int channels, int sample_rate) {
    src_channels_   = channels;
    src_sample_rate_ = sample_rate;

    // CoInitializeEx is idempotent — call on every thread that uses WASAPI
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    device_ = find_device(device_name);
    if (!device_) return -1;

    // Activate audio client
    IAudioClient *client = nullptr;
    HRESULT hr = as_device(device_)->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                               nullptr, reinterpret_cast<void **>(&client));
    if (FAILED(hr)) {
      BOOST_LOG(error) << "IAudioClient::Activate failed: 0x" << std::hex << hr;
      return -1;
    }
    audio_client_ = client;

    // Use the device's native mix format — avoids any format-mismatch crash.
    // Typically float32, may be stereo even if we are mono (we upmix below).
    WAVEFORMATEX *mix_fmt = nullptr;
    hr = client->GetMixFormat(&mix_fmt);
    if (FAILED(hr) || !mix_fmt) {
      BOOST_LOG(error) << "IAudioClient::GetMixFormat failed: 0x" << std::hex << hr;
      return -1;
    }

    dev_channels_    = static_cast<int>(mix_fmt->nChannels);
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

    BOOST_LOG(info) << "Virtual mic mix format: "
                    << mix_fmt->nChannels << "ch, "
                    << mix_fmt->nSamplesPerSec << "Hz, "
                    << mix_fmt->wBitsPerSample << "-bit "
                    << (dev_is_float_ ? "float" : "int");

    constexpr REFERENCE_TIME buf_duration = 2000000;  // 200 ms in 100-ns units
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, buf_duration, 0, mix_fmt, nullptr);
    CoTaskMemFree(mix_fmt);
    if (FAILED(hr)) {
      BOOST_LOG(error) << "IAudioClient::Initialize failed: 0x" << std::hex << hr;
      return -1;
    }

    hr = client->GetBufferSize(&buffer_frames_);
    if (FAILED(hr)) {
      BOOST_LOG(error) << "IAudioClient::GetBufferSize failed: 0x" << std::hex << hr;
      return -1;
    }

    IAudioRenderClient *render = nullptr;
    hr = client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void **>(&render));
    if (FAILED(hr)) {
      BOOST_LOG(error) << "GetService(IAudioRenderClient) failed: 0x" << std::hex << hr;
      return -1;
    }
    render_client_ = render;

    hr = client->Start();
    if (FAILED(hr)) {
      BOOST_LOG(error) << "IAudioClient::Start failed: 0x" << std::hex << hr;
      return -1;
    }

    active_ = true;
    BOOST_LOG(info) << "Virtual mic WASAPI stream started ("
                    << src_channels_ << "ch src → "
                    << dev_channels_ << "ch device, "
                    << src_sample_rate_ << "Hz)";
    return 0;
  }

  int virtual_mic_output_t::write_pcm(const opus_int16 *data, int frames) {
    if (!active_ || !render_client_ || !audio_client_) return -1;

    // Ensure COM is initialised on this thread (task-pool threads differ from init thread)
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IAudioClient       *client = as_audio_client(audio_client_);
    IAudioRenderClient *render = as_render_client(render_client_);

    UINT32 padding = 0;
    HRESULT hr = client->GetCurrentPadding(&padding);
    if (FAILED(hr)) {
      BOOST_LOG(warning) << "GetCurrentPadding failed: 0x" << std::hex << hr;
      return -1;
    }

    UINT32 available   = buffer_frames_ - padding;
    UINT32 write_frames = std::min(static_cast<UINT32>(frames), available);
    if (write_frames == 0) return 0;

    BYTE *buf = nullptr;
    hr = render->GetBuffer(write_frames, &buf);
    if (FAILED(hr)) {
      BOOST_LOG(warning) << "IAudioRenderClient::GetBuffer failed: 0x" << std::hex << hr;
      return -1;
    }

    // Convert mono int16 source → device format (upmix to dev_channels_ if needed)
    if (dev_is_float_) {
      auto *fbuf = reinterpret_cast<float *>(buf);
      for (UINT32 i = 0; i < write_frames; i++) {
        const float s = static_cast<float>(data[i]) / 32768.0f;
        for (int ch = 0; ch < dev_channels_; ch++) {
          fbuf[i * dev_channels_ + ch] = s;
        }
      }
    } else {
      auto *ibuf = reinterpret_cast<opus_int16 *>(buf);
      for (UINT32 i = 0; i < write_frames; i++) {
        for (int ch = 0; ch < dev_channels_; ch++) {
          ibuf[i * dev_channels_ + ch] = data[i];
        }
      }
    }

    render->ReleaseBuffer(write_frames, 0);
    return 0;
  }

}  // namespace platf::virtual_mic
