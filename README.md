# Voice Isolator RTX

**English** | [日本語](README.ja.md)

Voice Isolator RTX is a Windows x64 VST3 plug-in that reduces background noise in speech in real time. This repository contains the source for version 1.1.0. At the default 100% setting, you hear only the processed audio. The 80% and 50% settings blend in 20% and 50% of the original audio, respectively.

## Latency

**Low latency** reduces the delay reported to the DAW by 1 ms at 48 kHz. Both modes reserve a full maximum block so the delay stays constant when the DAW varies its block size during playback. Standard allows 10 ms for GPU processing and Low allows 9 ms.

| Maximum block at 48 kHz | Standard | Low |
|---|---|---|
| 128 samples | 2,176 samples (45.33 ms) | 2,128 samples (44.33 ms) |
| 256 samples | 2,304 samples (48.00 ms) | 2,256 samples (47.00 ms) |
| 512 samples | 2,560 samples (53.33 ms) | 2,512 samples (52.33 ms) |

If **Processed audio** drops below 100% under GPU load, switch to Standard. Switching in either direction asks the DAW to update plug-in delay compensation. Some hosts briefly interrupt playback during this update.

Version 1.1.0 opens projects saved with 1.0.2 using Standard latency and restores their noise reduction and bypass settings. Projects saved with 1.1.0 use a newer plug-in state format that 1.0.2 cannot read.

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

On a new PC, close your DAW and run `VoiceIsolatorRTX-1.0.2-Setup-x64.exe` from the Windows distribution. Setup installs engine 1.0.0 and downloads and verifies the model. Then copy the 1.1.0 VST3 bundle over the plug-in installed by setup. If the plug-in already shows **GPU engine → Ready**, the model is installed.

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
