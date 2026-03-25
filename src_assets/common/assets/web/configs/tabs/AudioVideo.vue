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

// Steam Streaming Microphone status
const steamMicAvailable = ref(false)
const loadingStatus = ref(false)

// Fetch Steam Streaming Microphone status
async function fetchSteamMicStatus() {
  if (props.platform !== 'windows') {
    return
  }
  
  loadingStatus.value = true
  
  try {
    const response = await fetch('/api/virtualmic/status')
    if (!response.ok) {
      throw new Error('Failed to fetch status')
    }
    
    const data = await response.json()
    steamMicAvailable.value = data.steam_mic_available || false
  } catch (e) {
    console.error('Failed to fetch Steam mic status:', e)
    steamMicAvailable.value = false
  } finally {
    loadingStatus.value = false
  }
}

// Fetch status on mount
onMounted(() => {
  if (props.platform === 'windows') {
    fetchSteamMicStatus()
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

        <!-- Microphone Passthrough Status -->
        <div class="mb-3">
          <label class="form-label">{{ $t('config.mic_passthrough') }}</label>
          <div class="alert" :class="steamMicAvailable ? 'alert-success' : 'alert-warning'">
            <div v-if="loadingStatus">
              Checking Steam Streaming Microphone...
            </div>
            <div v-else-if="steamMicAvailable">
              <strong>✓ Steam Streaming Microphone detected</strong><br>
              Microphone passthrough is enabled and ready.
            </div>
            <div v-else>
              <strong>⚠ Steam Streaming Microphone not found</strong><br>
              Install <a href="https://store.steampowered.com/about/" target="_blank">Steam</a> to enable microphone passthrough.
            </div>
          </div>
          <div class="form-text">
            {{ $t('config.mic_passthrough_desc') }}
          </div>
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