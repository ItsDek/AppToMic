# AppToMic

Send the audio of **one specific app** into a **virtual microphone** on Windows, and keep hearing it yourself.

Play music, a piano, a video or a game sound into Discord, Zoom, OBS or anything else that takes a mic, without sharing the rest of your PC's sound and without losing it from your own speakers.

<!-- Add a screenshot: save it as docs/screenshot.png and uncomment the next line -->
<!-- ![AppToMic](docs/screenshot.png) -->

## Features

- Captures **only the app you pick** (including its helper processes, e.g. browsers or Discord)
- You **still hear the app** on your speakers
- Clean signal: 48 kHz, 32-bit float, no compression or processing
- Volume slider (0–200%) and a live level meter
- Remembers your last app, device and volume
- One small `.exe`, no installation

## Requirements

- **Windows 11** (per-app audio capture isn't available on Windows 10)
- **[VB-Audio Virtual Cable](https://vb-audio.com/Cable/)** (free). It adds the virtual devices *CABLE Input* and *CABLE Output*.

## How to use

1. Install VB-Audio Virtual Cable and reboot if asked.
2. Download `AppToMic.exe` from the [Releases](../../releases) page.
3. Start playing sound in the app you want to send. It will show a ♪ in the list.
4. Open AppToMic, pick the app and press **Start**. *CABLE Input* is selected automatically.
5. In Discord, Zoom, OBS etc., choose **CABLE Output** as your microphone.

> **Windows SmartScreen warning?** The exe isn't code-signed, so Windows may show "Windows protected your PC". Click **More info → Run anyway**. Every release is built automatically by GitHub Actions from the source in this repo.

## Tips for music

Voice apps treat your "mic" like a voice and can damage music:

- **Discord:** Settings → Voice & Video → turn off *Noise Suppression*, *Echo Cancellation* and *Automatic Gain Control*.
- **Zoom:** Settings → Audio → enable *Original sound for musicians*.
- Keep AppToMic's volume at or below 100% to avoid clipping on loud passages.
- For the cleanest path, set both *CABLE Input* and *CABLE Output* to 48000 Hz in Control Panel → Sound → Properties → Advanced.

## How it works

AppToMic uses the Windows process-loopback API (`ActivateAudioInterfaceAsync` with `VAD\Process_Loopback`) to capture only the audio produced by the target process tree, then plays it into the playback device you choose (CABLE Input). VB-Cable turns that into a microphone.

## Building from source

With the MinGW-w64 cross compiler (Linux, WSL, or MSYS2 on Windows):

```sh
./build.sh
```

The exe lands in `build/AppToMic.exe`.

## License

[GPL-3.0-or-later](LICENSE). You can use, change and share AppToMic freely; if you distribute a modified version, it must also be GPL with its source code available.

VB-Audio Virtual Cable is a separate product by VB-Audio Software and isn't included.

---

Built with help from Claude (Anthropic).
