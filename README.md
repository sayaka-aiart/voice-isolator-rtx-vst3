# Voice Isolator RTX

**English** | [日本語](README.ja.md)

Voice Isolator RTX is a Windows x64 VST3 plug-in that reduces background noise in speech in real time. This repository contains the source for version 1.0.2. At the default 100% setting, you hear only the processed audio. The 80% and 50% settings blend in 20% and 50% of the original audio, respectively.

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

## License

The original plug-in code is licensed under the MIT License. The Steinberg VST3 SDK also uses the MIT License and is pinned to a specific revision as a Git submodule. VST is a trademark of Steinberg Media Technologies GmbH. [Plug-in license](LICENSE) · [Steinberg VST3 SDK](https://github.com/steinbergmedia/vst3sdk)

The plug-in uses Inworld Voice Isolator for speech isolation. The VST3 bundle includes its license and copyright notice. [Inworld Voice Isolator](https://github.com/inworld-ai/voice-isolator) · [Apache License 2.0](licenses/Inworld-LICENSE.txt) · [NOTICE](licenses/Inworld-NOTICE.txt)
