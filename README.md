# Home Assistant Voice PE — OpenAI Realtime fork

> **Customized fork** of `maxmaxme/home-assistant-voice-pe` (itself a fork of
> `esphome/home-assistant-voice-pe`). The Voice PE runs as a **thin client**: it
> streams microphone audio over a plain WebSocket to a backend add-on, which runs
> an **OpenAI Realtime API** session (`gpt-realtime-2`) for speech-to-speech and
> controls Home Assistant through the official
> **[Home Assistant MCP Server](https://www.home-assistant.io/integrations/mcp_server/)**
> integration. There is no Home Assistant `voice_assistant` pipeline on the audio
> path — STT, TTS and the LLM all live in the Realtime session inside the add-on.
>
> **Configs (pick one to flash):**
> - [`home-assistant-voice.realtime.yaml`](home-assistant-voice.realtime.yaml) — standard (DHCP).
> - [`home-assistant-voice.realtime.static-ip.yaml`](home-assistant-voice.realtime.static-ip.yaml) — identical, plus a fixed IP read from `secrets.yaml`.
>
> Companion backend add-on: **[xandervanerven/ha-openai-realtime](https://github.com/xandervanerven/ha-openai-realtime)**.

## What this fork changes vs. upstream

- **Custom `va_client` component** replaces the stock `voice_assistant`
  component: a thin WebSocket client (mic up / speaker down + an idle → listening
  → thinking → replying phase/LED state machine). All the voice intelligence
  runs in the backend add-on, not on the device.
- **Builds straight in ESPHome Builder**: `external_components` and the sound/
  model assets are pulled from GitHub, so you can paste a config into the ESPHome
  dashboard and build without a local checkout. This repo is **private**, so the
  component source URL carries a read-only token kept in `secrets.yaml` — it
  never lands in the committed config.
- **"stop" word + button interrupt**: say *"stop"* while the assistant is
  talking, or press the center button, to cancel the reply. This is the reliable
  way to interrupt.
- **Turn-based mic gating** (`barge_in: false`): the mic is closed while the
  assistant speaks, so its own TTS can't feed back through the (imperfect) XMOS
  AEC. Seamless handsfree barge-in was evaluated and **shelved** — it isn't
  achievable on this hardware without custom XMOS DSP work — so the "stop"
  word/button is the way to cut a reply short.
- **Audio-quality fixes**: a resampler cold-start "silence-prime" plus WiFi
  `power_save_mode: none` remove the start-of-reply crackle, and a ~600 ms mic
  pre-roll ring keeps the first word spoken during the wake chime from being lost.

## Setup (ESPHome Builder)

1. Install and configure the **OpenAI Realtime 2 Voice Agent** add-on from
   [xandervanerven/ha-openai-realtime](https://github.com/xandervanerven/ha-openai-realtime)
   (sets your OpenAI API key, the model, and the Home Assistant MCP connection).
   See that repo's documentation for the add-on side.
2. In the ESPHome dashboard, create a device from
   [`home-assistant-voice.realtime.yaml`](home-assistant-voice.realtime.yaml)
   (or the `…static-ip.yaml` variant if you want a fixed IP).
3. Provide these `secrets.yaml` keys — see
   [`secrets.yaml.example`](secrets.yaml.example) for the full template:
   - `wifi_ssid`, `wifi_password` — your Wi-Fi network.
   - `ota_password` — OTA upload password, so wireless updates keep working.
   - `api_key` — the device's Home Assistant API encryption key.
   - `va_components_repo` — the tokenized git URL for this private repo, e.g.
     `https://<TOKEN>@github.com/xandervanerven/home-assistant-voice-pe.git`
     (a GitHub fine-grained PAT with Contents: read on this repo only).
   - **Static-IP variant only:** `static_ip`, `gateway`, `subnet`, `dns1`, `dns2`.
   - Optionally override the `va_url` substitution if your add-on isn't reachable
     at `ws://homeassistant.local:8080/`.
4. Install / flash. The device connects to the add-on and you're ready.

---

Based on the ESPHome source of the [Home Assistant Voice: Preview Edition](https://www.home-assistant.io/voice-pe/).
See [the upstream documentation](https://voice-pe.home-assistant.io/) for hardware setup and troubleshooting.
