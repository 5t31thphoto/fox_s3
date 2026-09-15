# Next Steps to Production

1. **Audio**
   - Integrate official `M5Atomic-EchoBase` Arduino/ESP-IDF driver or write full ES8311 + I2S duplex.
   - Wire PTT (long-press) → record → upload WAV to Groq Whisper → get transcript.

2. **LLM**
   - Implement real Groq chat completions with streaming + tool_calls parsing.
   - For on-device: embed Needle 2 / MimiModel (or esp32-ai) binary and call its C API.
   - For stream: implement the wifi-llm style layer request protocol against your GitHub Pages weight host.

3. **Avatar**
   - 128×128 sprite sheet (idle / blink / talk / happy / think / radar).
   - Drive mouth openness from `audio_echo_last_amplitude()`.
   - Use M5Unified or esp_lcd + LVGL/simple blit.

4. **Tools**
   - Real BLE scan (esp_ble_gap) + BMI270 orientation → simple polar plot on the LCD.
   - Wi-Fi scan, IR RMT TX, gesture recognition.

5. **Web flasher**
   - Replace the demo “Build” with a real `workflow_dispatch` (GitHub App or personal token via a tiny Cloudflare Worker / Vercel function is the usual pattern).
   - After CI finishes, point `<esp-web-install-button>` at the published `manifest.json`.
   - Or keep a static latest build and only use the configurator for NVS defaults written at first boot via soft AP.

6. **Soft config**
   - Captive portal on first boot (or double-click menu → “Config”) so users can set Wi-Fi + API key without reflashing.

7. **CI**
   - Confirm partition table and merge_bin offsets for AtomS3R 8 MB flash.
   - Publish artifacts to GitHub Pages under `/builds/<run_id>/`.
