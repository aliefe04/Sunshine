/**
 * @file src/platform/windows/virtual_mic.cpp
 * @brief Definitions for Windows virtual microphone output.
 */

// standard includes
#include <cstring>

// platform includes
#include <Audioclient.h>
#include <mmdeviceapi.h>

// local includes
#include "src/config.h"
#include "src/logging.h"
#include "virtual_mic.h"

DEFINE_PROPERTYKEY(PKEY_Device_FriendlyName, 0xa45c254e, 0xdf1c, 0x4efd, 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0, 14);  // DEVPROP_TYPE_STRING

namespace platf::virtual_mic {
  using namespace std::literals;

  // RAII wrappers for COM objects
  template<class T>
  void Release(T *p) {
    if (p) {
      p->Release();
    }
  }

  template<class T>
  void co_task_free(T *p) {
    if (p) {
      CoTaskMemFree(p);
    }
  }

  using device_enum_t = std::unique_ptr<IMMDeviceEnumerator, decltype(&Release<IMMDeviceEnumerator>)>;
  using device_t = std::unique_ptr<IMMDevice, decltype(&Release<IMMDevice>)>;
  using audio_client_t = std::unique_ptr<IAudioClient, decltype(&Release<IAudioClient>)>;
  using render_client_t = std::unique_ptr<IAudioRenderClient, decltype(&Release<IAudioRenderClient>)>;
  using wave_format_t = std::unique_ptr<WAVEFORMATEX, decltype(&co_task_free<WAVEFORMATEX>)>;
  using wstring_t = std::unique_ptr<WCHAR, decltype(&co_task_free<WCHAR>)>;

  virtual_mic_output_t::~virtual_mic_output_t() {
    active_ = false;

    if (event_handle_) {
      CloseHandle(event_handle_);
      event_handle_ = nullptr;
    }

    if (render_client_) {
      Release(render_client_);
      render_client_ = nullptr;
    }

    if (audio_client_) {
      Release(audio_client_);
      audio_client_ = nullptr;
    }

    if (device_) {
      Release(device_);
      device_ = nullptr;
    }
  }

  void *virtual_mic_output_t::find_device(const std::string &device_name) {
    HRESULT status;
    device_enum_t device_enum(nullptr, Release<IMMDeviceEnumerator>);

    status = CoCreateInstance(
      __uuidof(MMDeviceEnumerator),
      nullptr,
      CLSCTX_ALL,
      __uuidof(IMMDeviceEnumerator),
      (void **) &device_enum
    );

    if (FAILED(status)) {
      BOOST_LOG(error) << "Couldn't create device enumerator: [0x"sv << util::hex(status).to_string_view() << ']';
      return nullptr;
    }

    // Enumerate audio render endpoints (virtual cable input is a render device)
    IMMDeviceCollection *collection = nullptr;
    status = device_enum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Couldn't enumerate audio endpoints: [0x"sv << util::hex(status).to_string_view() << ']';
      return nullptr;
    }

    UINT count = 0;
    collection->GetCount(&count);

    IMMDevice *found_device = nullptr;

    for (UINT i = 0; i < count; i++) {
      IMMDevice *device = nullptr;
      status = collection->Item(i, &device);
      if (FAILED(status)) {
        continue;
      }

      // Get device friendly name
      IPropertyStore *props = nullptr;
      status = device->OpenPropertyStore(STGM_READ, &props);
      if (FAILED(status)) {
        device->Release();
        continue;
      }

      PROPVARIANT name_prop;
      PropVariantInit(&name_prop);
      status = props->GetValue(PKEY_Device_FriendlyName, &name_prop);
      if (SUCCEEDED(status) && name_prop.vt == VT_LPWSTR) {
        // Convert wide string to narrow string
        char narrow_name[256] = {0};
        WideCharToMultiByte(CP_UTF8, 0, name_prop.pwszVal, -1, narrow_name, sizeof(narrow_name), nullptr, nullptr);

        std::string device_name_str(narrow_name);

        // Check if this is the device we're looking for
        bool match = false;
        if (device_name.empty()) {
          // Auto-detect VB-Cable
          match = device_name_str.find("CABLE Input") != std::string::npos ||
                  device_name_str.find("VB-Audio") != std::string::npos;
        } else {
          match = device_name_str.find(device_name) != std::string::npos;
        }

        if (match) {
          BOOST_LOG(info) << "Found virtual mic device: "sv << device_name_str;
          found_device = device;
          PropVariantClear(&name_prop);
          props->Release();
          break;
        }
      }

      PropVariantClear(&name_prop);
      props->Release();
      device->Release();
    }

    collection->Release();
    return found_device;
  }

  int virtual_mic_output_t::init(const std::string &device_name, int channels, int sample_rate) {
    HRESULT status;

    channels_ = channels;
    sample_rate_ = sample_rate;

    // Find the virtual device
    device_ = find_device(device_name);
    if (!device_) {
      if (device_name.empty()) {
        BOOST_LOG(error) << "No VB-Audio Virtual Cable found. Install from https://vb-audio.com/Cable/"sv;
      } else {
        BOOST_LOG(error) << "Virtual mic device not found: "sv << device_name;
      }
      return -1;
    }

    // Activate audio client
    IAudioClient *audio_client_raw = nullptr;
    status = ((IMMDevice *) device_)->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **) &audio_client_raw);

    if (FAILED(status)) {
      BOOST_LOG(error) << "Couldn't activate audio client: [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }
    audio_client_ = audio_client_raw;

    // Get the mix format
    WAVEFORMATEX *mix_format_raw = nullptr;
    status = audio_client_raw->GetMixFormat(&mix_format_raw);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Couldn't get mix format: [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    BOOST_LOG(info) << "Virtual mic mix format: "sv << mix_format_raw->nChannels << " channels, "
                    << mix_format_raw->nSamplesPerSec << " Hz, "
                    << mix_format_raw->wBitsPerSample << " bits";

    // Create our desired format (16-bit PCM, mono/stereo, 48kHz)
    WAVEFORMATEX desired_format = {};
    desired_format.wFormatTag = WAVE_FORMAT_PCM;
    desired_format.nChannels = channels_;
    desired_format.nSamplesPerSec = sample_rate_;
    desired_format.wBitsPerSample = 16;
    desired_format.nBlockAlign = desired_format.nChannels * desired_format.wBitsPerSample / 8;
    desired_format.nAvgBytesPerSec = desired_format.nSamplesPerSec * desired_format.nBlockAlign;
    desired_format.cbSize = 0;

    // Check if the format is supported
    WAVEFORMATEX *closest_format = nullptr;
    status = audio_client_raw->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, &desired_format, &closest_format);
    if (status == S_FALSE && closest_format) {
      // Use the closest format
      BOOST_LOG(info) << "Using closest format: "sv << closest_format->nChannels << " channels, "
                      << closest_format->nSamplesPerSec << " Hz";
      CoTaskMemFree(closest_format);
    } else if (FAILED(status)) {
      BOOST_LOG(warning) << "Desired format not supported, using mix format"sv;
      desired_format = *mix_format_raw;
    }

    CoTaskMemFree(mix_format_raw);

    // Initialize audio client
    // Use 20ms buffer (960 samples at 48kHz)
    REFERENCE_TIME buffer_duration = 200000;  // 20ms in 100ns units

    status = audio_client_raw->Initialize(
      AUDCLNT_SHAREMODE_SHARED,
      AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
      buffer_duration,
      0,
      &desired_format,
      nullptr
    );

    if (FAILED(status)) {
      BOOST_LOG(error) << "Couldn't initialize audio client: [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    // Get render client
    IAudioRenderClient *render_client_raw = nullptr;
    status = audio_client_raw->GetService(__uuidof(IAudioRenderClient), (void **) &render_client_raw);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Couldn't get render client: [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }
    render_client_ = render_client_raw;

    // Create event handle
    HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!event) {
      BOOST_LOG(error) << "Couldn't create event handle"sv;
      return -1;
    }
    event_handle_ = event;

    // Set event handle
    status = audio_client_raw->SetEventHandle(event);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Couldn't set event handle: [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    // Start the audio client
    status = audio_client_raw->Start();
    if (FAILED(status)) {
      BOOST_LOG(error) << "Couldn't start audio client: [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    active_ = true;
    BOOST_LOG(info) << "Virtual mic output initialized successfully"sv;
    return 0;
  }

  int virtual_mic_output_t::write_pcm(const int16_t *pcm_data, int samples) {
    if (!active_ || !render_client_) {
      return -1;
    }

    HRESULT status;
    auto *audio_client = (IAudioClient *) audio_client_;
    auto *render_client = (IAudioRenderClient *) render_client_;

    // Get buffer size
    UINT32 buffer_size = 0;
    status = audio_client->GetBufferSize(&buffer_size);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Couldn't get buffer size: [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    // Get available buffer space
    UINT32 padding = 0;
    status = audio_client->GetCurrentPadding(&padding);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Couldn't get current padding: [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    UINT32 available = buffer_size - padding;
    if (available < (UINT32) samples) {
      // Buffer full, wait for event
      WaitForSingleObject(event_handle_, 20);
    }

    // Get buffer
    BYTE *data = nullptr;
    status = render_client->GetBuffer(samples, &data);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Couldn't get buffer: [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    // Copy int16 data directly to buffer
    memcpy(data, pcm_data, samples * channels_ * sizeof(int16_t));

    // Release buffer
    status = render_client->ReleaseBuffer(samples, 0);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Couldn't release buffer: [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    return 0;
  }

}  // namespace platf::virtual_mic
