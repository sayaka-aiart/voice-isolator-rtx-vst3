# Voice Isolator RTX

**English** | [日本語](README.ja.md)

This repository contains the source for Voice Isolator RTX, a Windows x64 VST3 plug-in that reduces background noise in speech in real time.

Version 1.3.0 adds **Dual mono**. For a binaural microphone, route its left and right inputs to one stereo track, insert one Voice Isolator RTX, and turn on **Dual mono**. The plug-in processes each side independently. With the button off (the default), it processes the average of the inputs and sends the result to both sides. Select the mode before recording or going live. Changing it briefly mutes the output, even with Bypass on. The DAW saves the setting with the project.

Select **5 ms** or **10 ms** under **Extra synthesis advance** to reduce the plug-in delay reported to your DAW by that amount at 48 kHz. The default **0 ms** uses the same synthesis timing as version 1.1.0.

**Noise reduction** sets the mix. At 100% (default), you hear only the processed audio. The 80% and 50% settings blend in 20% and 50% of the original audio, respectively.

## Latency modes

All three presets use the same model and process audio in 10 ms steps. The 5 ms and 10 ms presets shorten the overlap between synthesized frames so the plug-in can output the audio sooner. This can change the texture of speech and how much of a brief background sound remains.

With **Low latency** off (Standard, the default), the GPU has a 10 ms processing allowance. Turning it on reduces that allowance to 9 ms, saving another 1 ms at 48 kHz with any synthesis preset. The table shows the delay reported to the DAW at **48 kHz with a maximum block size of 128 samples**.

| Extra synthesis advance | Typical use | Standard | Low latency |
|---|---|---:|---:|
| 0 ms (default) | Record or mix with the original 1.1.0 sound. | 45.33 ms | 44.33 ms |
| 5 ms | Reduce the delay when monitoring your voice, with moderate synthesis overlap. | 40.33 ms | 39.33 ms |
| 10 ms | Use the shortest monitoring delay. Compare the sound with 0 ms on your own voice. | 35.33 ms | 34.33 ms |

All modes reserve a full maximum host block for variable callbacks. At 48 kHz, a 256-sample maximum adds 2.67 ms to the 128-sample figure, and a 512-sample maximum adds 8 ms. Microphone-to-headphone monitoring also includes the audio interface and driver round trip.

If **Processed audio** drops below 100% under GPU load, switch to Standard. Changing either latency control asks the DAW to update plug-in delay compensation. Playback can briefly pause while the new synthesis stream fills.

Projects saved before 1.3.0 open with **Dual mono** off. Projects saved before 1.2.0 open with **0 ms** extra advance. Saved noise reduction, bypass and latency settings are retained. Projects predating the Low latency control use Standard. With **Dual mono** off, settings saved at 0 ms can reopen in 1.1.0, and settings saved at 5 or 10 ms require at least 1.2.0. Settings saved with **Dual mono** on require 1.3.0 or later.

## Build

On Windows, install Git, CMake 3.25 or later, and Visual Studio 2022 with the **Desktop development with C++** workload. Include the MSVC v143 toolset and the Windows SDK. Run the following commands in PowerShell.

```powershell
git clone https://github.com/sayaka-aiart/voice-isolator-rtx-vst3.git
cd voice-isolator-rtx-vst3
git submodule update --init --depth 1 third_party/vst3sdk
git -C third_party/vst3sdk submodule update --init --depth 1 base cmake pluginterfaces public.sdk
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target VoiceIsolatorRTX --parallel
```

The build produces `build/VST3/Release/VoiceIsolatorRTX.vst3/`. Copy that entire folder into a VST3 directory that your DAW scans, then rescan your plug-ins.

## Audio processing requirements

You can build the plug-in on a Windows PC without a GPU. To process audio in a DAW, you need a compatible NVIDIA GPU and version 1.0.0 of the Voice Isolator RTX inference engine. If you already have the release installed, the plug-in reuses its private Python runtime, CUDA libraries, and model.

To locate the engine, the plug-in reads the `EngineRoot` value under `HKCU\Software\Local Audio Lab\Voice Isolator RTX` in the current user's Windows registry. If no engine is registered, **GPU engine** shows **Unavailable** and the effect outputs silence.

## Download the inference model

Extract `VoiceIsolatorRTX-1.3.0-Windows-x64.zip`, close your DAW, and run `VoiceIsolatorRTX-1.3.0-Setup-x64.exe`. On a new PC, setup installs the VST3 and engine 1.0.0 and downloads and verifies the model. When upgrading in the same location, setup reuses the verified engine and model. To use a build from this repository, copy your entire built VST3 folder over the installed one, then reopen the DAW.

Voice Isolator RTX uses Inworld's realtime checkpoint, `denoiser_realtime.safetensors` (232,780,800 bytes, about 233 MB). To get the model separately, save it to an empty folder using the download link, or open PowerShell there and run the command below. [Download the model from Hugging Face](https://huggingface.co/inworld/voice-isolator/resolve/9b0285b3fe8403aa8e2f1dbb2165a80a4294c970/denoiser_realtime.safetensors)

```powershell
curl.exe --fail --location --retry 2 --output denoiser_realtime.safetensors "https://huggingface.co/inworld/voice-isolator/resolve/9b0285b3fe8403aa8e2f1dbb2165a80a4294c970/denoiser_realtime.safetensors"
if ($LASTEXITCODE -ne 0) { throw 'Model download failed.' }
```

Verify either download by running the following in the folder containing the file. `True` means the SHA-256 matches. If you get `False`, download the file again before using it.

```powershell
$expected = '76598f05523c0669e76562a8b2f31c9d395fca316e80c907ee11775e7497ae0f'
(Get-FileHash -LiteralPath '.\denoiser_realtime.safetensors' -Algorithm SHA256 -ErrorAction Stop).Hash -eq $expected
```

To restore a missing or damaged model in an existing installation, close your DAW. Run the following in 64-bit PowerShell to find the exact destination, then copy the verified file there. Reopen the plug-in and check that **GPU engine** shows **Ready**.

```powershell
$key = 'HKCU:\Software\Local Audio Lab\Voice Isolator RTX'
$engine = Get-ItemPropertyValue -LiteralPath $key -Name EngineRoot -ErrorAction Stop
Join-Path $engine 'inworld\models\denoiser_realtime.safetensors'
```

## License

The original plug-in code is licensed under the MIT License. The Steinberg VST3 SDK also uses the MIT License and is pinned to a specific revision as a Git submodule. VST is a trademark of Steinberg Media Technologies GmbH. [Plug-in license](LICENSE) · [Steinberg VST3 SDK](https://github.com/steinbergmedia/vst3sdk)

The plug-in uses Inworld Voice Isolator for speech isolation. The VST3 bundle includes its license and copyright notice. [Inworld Voice Isolator](https://github.com/inworld-ai/voice-isolator) · [Apache License 2.0](licenses/Inworld-LICENSE.txt) · [NOTICE](licenses/Inworld-NOTICE.txt)
