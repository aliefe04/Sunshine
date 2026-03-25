<script setup>
import {ref, onMounted, watch} from 'vue'
import {$tp} from '../../platform-i18n'
import PlatformLayout from '../../PlatformLayout.vue'
import AdapterNameSelector from './audiovideo/AdapterNameSelector.vue'
import DisplayOutputSelector from './audiovideo/DisplayOutputSelector.vue'
import DisplayDeviceOptions from "./audiovideo/DisplayDeviceOptions.vue";
import DisplayModesSettings from "./audiovideo/DisplayModesSettings.vue";
import Checkbox from "../../Checkbox.vue";

const props = defineProps([
  'platform',
  'config',
])

const config = ref(props.config)

// Virtual microphone device state
const virtualMicDevices = ref([])
const steamMicAvailable = ref(false)
const anyDeviceAvailable = ref(false)
const loadingDevices = ref(false)
const deviceError = ref('')

// Fetch available virtual mic devices
async function fetchVirtualMicDevices() {
  if (props.platform !== 'windows') {
    return
  }
  
  loadingDevices.value = true
  deviceError.value = ''
  
  try {
    const response = await fetch('/api/virtualmic/devices')
    if (!response.ok) {
      throw new Error('Failed to fetch devices')
    }
    
    const data = await response.json()
    virtualMicDevices.value = data.devices || []
    steamMicAvailable.value = data.steam_mic_available || false
    anyDeviceAvailable.value = data.any_available || false
  } catch (e) {
    deviceError.value = e.message
    console.error('Failed to fetch virtual mic devices:', e)
  } finally {
    loadingDevices.value = false
  }
}

// Watch for mic passthrough being enabled
watch(() => config.value.mic_passthrough, (newVal) => {
  if (newVal === 'enabled' && props.platform === 'windows') {
    fetchVirtualMicDevices()
  }
})

// Fetch devices on mount if mic passthrough is already enabled
onMounted(() => {
  if (config.value.mic_passthrough === 'enabled' && props.platform === 'windows') {
    fetchVirtualMicDevices()
  }
})
</script>

<template>
  <div id="audio-video" class="config-page">
    <!-- Audio Sink -->
    <div class="mb-3">
      <label for="audio_sink" class="form-label">{{ $t('config.audio_sink') }}</label>
      <input type="text" class="form-control" id="audio_sink"
             :placeholder="$tp('config.audio_sink_placeholder', 'alsa_output.pci-0000_09_00.3.analog-stereo')"
             v-model="config.audio_sink" />
      <div class="form-text">
        {{ $tp('config.audio_sink_desc') }}<br>
        <PlatformLayout :platform="platform">
          <template #windows>
            <pre>tools\audio-info.exe</pre>
          </template>
          <template #freebsd>
            <pre>pacmd list-sinks | grep "name:"</pre>
            <pre>pactl info | grep Source</pre>
          </template>
          <template #linux>
            <pre>pacmd list-sinks | grep "name:"</pre>
            <pre>pactl info | grep Source</pre>
          </template>
          <template #macos>
            <a href="https://github.com/mattingalls/Soundflower" target="_blank">Soundflower</a><br>
            <a href="https://github.com/ExistentialAudio/BlackHole" target="_blank">BlackHole</a>.
          </template>
        </PlatformLayout>
      </div>
    </div>


    <PlatformLayout :platform="platform">
      <template #windows>
        <!-- Virtual Sink -->
        <div class="mb-3">
          <label for="virtual_sink" class="form-label">{{ $t('config.virtual_sink') }}</label>
          <input type="text" class="form-control" id="virtual_sink" :placeholder="$t('config.virtual_sink_placeholder')"
                 v-model="config.virtual_sink" />
          <div class="form-text">{{ $t('config.virtual_sink_desc') }}</div>
        </div>

        <!-- Install Steam Audio Drivers -->
        <Checkbox class="mb-3"
                  id="install_steam_audio_drivers"
                  locale-prefix="config"
                  v-model="config.install_steam_audio_drivers"
                  default="true"
        ></Checkbox>
      </template>
    </PlatformLayout>

    <!-- Microphone Passthrough (Windows only) -->
    <PlatformLayout :platform="platform">
      <template #windows>
        <Checkbox class="mb-3"
                  id="mic_passthrough"
                  locale-prefix="config"
                  v-model="config.mic_passthrough"
                  default="false"
        ></Checkbox>

        <div class="mb-3" v-if="config.mic_passthrough === 'enabled'">
          <!-- Device Status -->
          <div class="alert" :class="anyDeviceAvailable ? 'alert-success' : 'alert-warning'" v-if="!loadingDevices">
            <div v-if="steamMicAvailable">
              <strong>✓ Steam Streaming Microphone detected</strong> - Recommended
            </div>
            <div v-else-if="anyDeviceAvailable">
              <strong>✓ Virtual audio device detected</strong> - VB-Cable or similar
            </div>
            <div v-else>
              <strong>⚠ No virtual audio device found</strong><br>
              Install <a href="https://store.steampowered.com/about/" target="_blank">Steam</a> (recommended) or 
              <a href="https://vb-audio.com/Cable/" target="_blank">VB-Cable</a> for microphone passthrough.
            </div>
          </div>

          <!-- Device Selector -->
          <label for="mic_virtual_device" class="form-label">{{ $t('config.mic_virtual_device') }}</label>
          
          <!-- Show dropdown if devices are available -->
          <select v-if="virtualMicDevices.length > 0" 
                  class="form-select" 
                  id="mic_virtual_device"
                  v-model="config.mic_virtual_device">
            <option value="">Auto-detect (Steam preferred)</option>
            <option v-for="device in virtualMicDevices" 
                    :key="device.name" 
                    :value="device.name">
              {{ device.name }}
              <span v-if="device.is_steam"> (Steam - Recommended)</span>
              <span v-else-if="device.is_vb_cable"> (VB-Cable)</span>
            </option>
          </select>
          
          <!-- Show text input if no devices found or loading -->
          <input v-else type="text" class="form-control" id="mic_virtual_device"
                 placeholder="Steam Streaming Microphone"
                 v-model="config.mic_virtual_device" />
          
          <div class="form-text">{{ $t('config.mic_virtual_device_desc') }}</div>
        </div>
      </template>
    </PlatformLayout>

    <!-- Disable Audio -->
    <Checkbox class="mb-3"
              id="stream_audio"
              locale-prefix="config"
              v-model="config.stream_audio"
              default="true"
    ></Checkbox>

    <AdapterNameSelector
        :platform="platform"
        :config="config"
    />

    <DisplayOutputSelector
      :platform="platform"
      :config="config"
    />

    <DisplayDeviceOptions
      :platform="platform"
      :config="config"
    />

    <!-- Display Modes -->
    <DisplayModesSettings
        :platform="platform"
        :config="config"
    />

  </div>
</template>

<style scoped>
.alert {
  padding: 0.75rem 1.25rem;
  margin-bottom: 1rem;
  border: 1px solid transparent;
  border-radius: 0.375rem;
}

.alert-success {
  color: #0f5132;
  background-color: #d1e7dd;
  border-color: #badbcc;
}

.alert-warning {
  color: #664d03;
  background-color: #fff3cd;
  border-color: #ffecb5;
}

.alert a {
  color: inherit;
  text-decoration: underline;
}
</style>