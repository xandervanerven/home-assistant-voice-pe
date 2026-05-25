# Voice PE → voice-assistant direct streaming

**Status:** Draft
**Date:** 2026-05-25
**Affects:** `home-assistant-voice-pe`, `voice-assistant`, `home-infra`

## Problem

Current voice pipeline goes:

```
Voice PE → HA voice_assistant (STT via OpenAI gpt-4o-mini-transcribe)
        → HTTP Conversation Agent → voice-assistant /assist (LLM, tools via HA MCP)
        → HA ElevenLabs TTS → streaming URL → Voice PE plays
```

End-to-end latency from end-of-speech to first audio out is multiple seconds
because each hop is blocking: STT waits for full utterance, LLM waits for
full STT result, TTS waits for full LLM result. HA orchestrates everything
synchronously.

## Goal

Cut latency to sub-second first-audio by:

1. Bypassing Home Assistant entirely for voice traffic.
2. Using OpenAI Realtime API (audio-in/audio-out single WebSocket) so STT,
   LLM, and TTS overlap on the server side.
3. Letting voice-assistant act as a thin bridge between the device and
   Realtime, while keeping HA only for tool execution via the existing MCP
   integration.

Voice PE on this design is a voice-only appliance. Music playback, media_player
features, timer announcements, and ducking are out of scope — this device is
used solely as a voice assistant; music plays on other speakers.

## Non-goals

- Replacing music playback on Voice PE — explicitly dropped.
- Removing Home Assistant from the broader stack — HA still owns devices,
  automations, MCP tools.
- Local STT/LLM/TTS — everything stays in the cloud via OpenAI Realtime.
- Multi-device fan-out — single Voice PE for now; per-device session keys
  later if needed.
- Preserving HA's `voice_assistant` entity, `media_player` entity, or
  `assist_pipeline` integration on the firmware side.

## Architecture

```
Voice PE (ESP32-S3 + XMOS)            voice-assistant (Pi)         OpenAI
─────────────────────────             ────────────────────         ──────
  XMOS DSP (AEC/NS/IC/AGC)
   │
   │  PCM 16 kHz mono ch0
   ▼
  micro_wake_word ──(trigger)──► va_client
                                  │
                                  │  Opus 16 kHz frames (audio in)
                                  │  JSON control msgs
                                  ▼
                                 [WS server :3001 /voice]
                                  │
                                  ▼
                                 RealtimeBridge
                                  │ PCM16 24 kHz ──────► [WS] Realtime
                                  │                      ◄────── (audio/text/tool_call)
                                  │
                                  │ tool_call ─► HA MCP client ─► Home Assistant
                                  │ tool_result ◄─────────────────
                                  │
                                  │ PCM16 24 kHz (audio out)
                                  ▼
                               (resample, optional Opus encode)
                                  │
   ◄──── audio binary chunks ─────┘
   ◄──── JSON phase messages ─────
  speaker queue + LED phases
```

### Component boundaries

- **`va_client` (firmware C++ component)** — owns the WS connection, audio
  framing, codec, and LED phase rendering. Knows nothing about HA, OpenAI,
  or tools.
- **WS server (`/voice`) in voice-assistant** — auth, message routing,
  audio resampling, optional Opus codec. Stateless w.r.t. conversation;
  state lives in `RealtimeBridge`.
- **`RealtimeBridge`** — one instance per device session. Owns the OpenAI
  Realtime WS, prompts, tool schema, and forwards audio/tool events.
- **HA MCP client** — already exists in voice-assistant. Reused unchanged.

## Firmware changes (`home-assistant-voice-pe`)

### Removed from `home-assistant-voice.yaml`

| Block | Approx lines | Reason |
| --- | --- | --- |
| `voice_assistant:` | 1788–1894 | Replaced by `va_client` |
| `external_media_player` + sources | 1545–1669 | No music/announce playback |
| `http_announcement_source` action chains | various | TTS arrives as WS chunks, not URL |
| `api:` (ESPHome native API) | 108 | Device no longer registers with HA |

Kept:

- `i2s_audio`, `microphone` (dual ch), `speaker` (single output)
- `voice_kit` custom component (XMOS DSP control)
- `micro_wake_word` (wake words + "stop" interrupt)
- `wifi`, `improv`, `ota`, LED logic, buttons

### New component `esphome/components/va_client/`

Files:

```
va_client/
  __init__.py         # codegen for yaml config
  va_client.h         # class definition
  va_client.cpp       # WS client + audio pipeline glue
  opus_codec.cpp      # libopus wrappers (encode 16k, decode 24k)
  audio_queue.h       # ring buffer for speaker
```

YAML config example:

```yaml
va_client:
  id: va
  url: wss://va.local:3001/voice
  token: !secret va_device_token
  audio_in_codec: opus      # opus | pcm
  audio_out_codec: opus     # opus | pcm
  microphone: mic_source_ch0
  speaker: speaker_out
  on_phase:
    - lambda: |-
        // route phase to LED effects
```

Responsibilities:

1. Maintain WS connection (idle-keep open OR connect-on-wake; M2 starts with
   connect-on-wake, M3 evaluates idle-keep for latency).
2. Auth via `Authorization: Bearer <token>` header on upgrade.
3. On `micro_wake_word` trigger: send `{"type":"start"}`, begin streaming
   mic frames.
4. Encode mic ch0 as Opus 16 kHz mono 20 ms frames (or raw PCM in M2).
5. Decode incoming binary frames → push into speaker ring buffer.
6. Parse JSON control frames; emit ESPHome triggers for LED phase changes.
7. Local "stop" wake word → send `{"type":"interrupt"}` + flush speaker buffer.
8. Reconnect with exponential backoff (1s, 2s, 5s, 10s capped). On hard
   failure: play local `error.flac` and show red-X LED.

Dependencies: `libopus` via PlatformIO. WS client via `esp_websocket_client`
from esp-idf (already pulled in by the ESPHome base).

### Echo cancellation note

XMOS does AEC using a reference signal taken from the I2S speaker output bus.
Since `va_client` writes PCM into the same speaker via the ESPHome `speaker`
component, the reference path is preserved as long as we feed audio through
`speaker_out` (not bit-banged to GPIOs). M2 must verify barge-in still works
(say "okay nabu" while TTS is playing → wake word fires, interrupt sent).

## voice-assistant changes

### New WS endpoint

- Port `3001` (separate from existing `:3000` `/assist` HTTP).
- Path `/voice`.
- Auth: `Authorization: Bearer <VA_DEVICE_TOKEN>` env-configured.

### `RealtimeBridge` class

One instance per active device WS. Pseudocode:

```ts
class RealtimeBridge {
  constructor(deviceWs, mcpClient, config) { ... }

  async start() {
    this.openaiWs = await connectRealtime({
      model: 'gpt-realtime',
      instructions: config.systemPrompt,
      tools: await this.mcpClient.listTools(),  // converted to Realtime tool schema
      voice: 'alloy',  // Realtime voice; ElevenLabs Artspace is not available in this path
      input_audio_format: 'pcm16',
      output_audio_format: 'pcm16',
      turn_detection: { type: 'server_vad' },
    });

    this.pipeAudioDeviceToOpenAi();
    this.pipeAudioOpenAiToDevice();
    this.handleControlMessages();
    this.handleToolCalls();  // run via mcpClient, feed result back
  }
}
```

Audio path:
- Device sends Opus 16 kHz → decode → upsample 16→24 → send to Realtime as
  PCM16 base64 chunks.
- Realtime emits PCM16 24 kHz audio deltas → downsample 24→16 (optional, if
  device prefers 16 kHz playback) or pass through 24 kHz → encode Opus →
  send to device.

Control messages from va to device:
- `{type:"phase", value:"listening"}` — on Realtime `input_audio_buffer.speech_started`
- `{type:"phase", value:"thinking"}` — on `response.created`
- `{type:"phase", value:"replying"}` — on first `response.audio.delta`
- `{type:"phase", value:"idle"}` — on `response.done`
- `{type:"error", message:"..."}` — any unrecoverable error

Control messages from device:
- `{type:"start"}` — wake word fired, begin session (no-op if already connected)
- `{type:"interrupt"}` — local stop word; bridge issues `response.cancel`
  and `input_audio_buffer.clear`, drops outgoing audio queue.
- `{type:"ping"}` / `{type:"pong"}` — keepalive.

### Reuse from existing va

- `OpenAiAgent` system prompt construction.
- HA MCP client and tool result handling.
- Conversation memory layer (per-device key = device token claim or fixed
  for single-device deployment).

### Dependencies

- `ws` (already in va or added).
- `@discordjs/opus` or `@evan/opus` for Opus codec (native binding, prebuilt
  arm64 wheel preferred).
- `node-libsamplerate` or simple linear resampler 16↔24.

## home-infra changes

- `docker-compose.yml`: expose port 3001 on the `voice-assistant` service.
- `.env`: new `VA_DEVICE_TOKEN`.
- No new systemd units needed; the existing `voice-assistant.service`
  covers the new endpoint.
- CLAUDE.md updates: note the new WS contract between Voice PE and va.

## Wire protocol

### Audio (binary frames)

- Device → va: Opus, 16 kHz mono, 20 ms frame per WS message. Fallback raw
  PCM16 little-endian if Opus disabled in yaml (M2 default).
- va → device: Opus or raw PCM16 24 kHz mono, ~40 ms chunks. Format
  negotiated via initial JSON handshake (`{"type":"hello", audio_out:"opus"}`
  from device).

### Control (text frames, UTF-8 JSON)

All control messages have a `type` field. Unknown types are logged and
ignored (forward-compat).

### Auth

Bearer token in `Authorization` header at WS upgrade. Token is a 32+ byte
random hex string shared between firmware substitution and va env. Local
network only in M1–M4; WSS optional in M5.

### Sequence: happy path

```
device                                 va                    Realtime
──────                                 ──                    ────────
[wake word]
─── {"type":"start"} ─────────────►
                                      ── ws connect ────────►
                                      ◄── session.created ───
─── opus frame ───────────────────►
                                      ── audio_buffer.append ─►
─── opus frame ───────────────────►
                                      ── audio_buffer.append ─►
                                      ◄── speech_started ────
◄── {"type":"phase","value":"listening"}
                                      ◄── speech_stopped ────
                                      ◄── response.created ──
◄── {"type":"phase","value":"thinking"}
                                      ◄── tool_call ─────────
                                      ── (HA MCP call) ──────
                                      ── tool_result ────────►
                                      ◄── response.audio.delta
◄── {"type":"phase","value":"replying"}
◄── opus chunk ───────────────────
◄── opus chunk ───────────────────
                                      ◄── response.done ─────
◄── {"type":"phase","value":"idle"}
```

### Sequence: interrupt

```
device                                 va                    Realtime
[wake word "stop" while replying]
─── {"type":"interrupt"} ─────────►
   [flush local speaker buffer]      ── response.cancel ───►
                                      ── input_audio_buffer.clear ─►
                                      [drop outgoing audio queue]
◄── {"type":"phase","value":"listening"}
```

## Error handling

| Failure | Response |
| --- | --- |
| va WS unreachable | Backoff reconnect 1/2/5/10s. Red-X LED. Play `error.flac`. |
| Realtime WS drops mid-response | va sends `phase:idle` + `error`. Device tushes replying LED. |
| Tool call throws | va returns error string to Realtime as tool_result. Model speaks the error. |
| Realtime emits malformed tool schema | va whitelists tool list at startup; oversized schemas pruned. |
| Auth fails | WS closed with 4401. Device retries with backoff and shows red-X. |
| Device disconnects mid-stream | va closes Realtime, frees session. No persistence needed. |

## Milestones

1. **M1 — va-side skeleton.** WS server + RealtimeBridge + smoke test with
   a CLI client streaming a WAV file. Latency metrics in logs.
2. **M2 — talking prototype.** New `va_client` component (raw PCM only).
   Separate yaml `home-assistant-voice.va-direct.yaml`. End-to-end speech
   round-trip works. Old pipeline still available on the original yaml.
3. **M3 — Opus + interrupt + reliability.** Opus both directions, local
   "stop" interrupt, reconnect with backoff, error.flac on failure.
4. **M4 — production-ready.** OTA firmware, docker-compose update for
   port 3001 and `VA_DEVICE_TOKEN`, optional WSS. Side-by-side with old
   pipeline as fallback.
5. **M5 — cleanup.** Delete old yaml, rename va-direct → main. Update
   CLAUDE.md in umbrella, firmware repo, va repo. Update firmware README
   to note the fork is no longer drop-in compatible with HA's standard
   Voice PE pipeline.

## Testing

- **Latency benchmark** at each milestone: ten fixed utterances ("turn on
  kitchen light", "what time is it", "what's the weather", etc). Log:
  `t_wake → t_first_audio_out` and `t_speech_end → t_first_audio_out`.
- **Stress**: five back-to-back utterances with no pause. Interrupt must
  cut off replies cleanly.
- **Network failure**: stop the va container during reply. Device must
  recover within 10 s and stay usable after va restart.
- **Tool latency**: same utterance with and without tool calls; baseline
  the tool path overhead.
- **Barge-in**: speak wake word while TTS playing; XMOS AEC must still
  produce a clean signal (verify by transcript accuracy on the cut-in).

## Open questions / risks

- **ESP32-S3 CPU for Opus encode** alongside `micro_wake_word`. If CPU is
  too tight, stay on raw PCM end-to-end. Local LAN bandwidth is plentiful.
- **HA MCP tool schema size.** Some HA service schemas are large; Realtime
  tool definitions may need pruning or per-tool descriptions to fit. Plan
  to whitelist relevant domains (light, climate, switch, scene, script,
  media_player on other speakers, etc.) rather than expose every entity.
- **Voice quality vs ElevenLabs "Artspace".** Realtime voices are different
  in timbre. If unacceptable, future work could split STT+LLM via Realtime
  but route text deltas to ElevenLabs streaming TTS — more latency but
  preserves the voice.
- **Per-device session memory.** Single device today, but the session key
  scheme (token claim → memory key) should not assume one device forever.
- **AEC reference signal.** Confirm in M2 that XMOS AEC still has a valid
  reference path when audio is written by `va_client` → `speaker` rather
  than the old `external_media_player` chain.
