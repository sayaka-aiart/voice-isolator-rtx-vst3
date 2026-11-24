# Voice Isolator RTX

話し声の背景ノイズをリアルタイムに抑えるWindows x64用VST3プラグインです。このリポジトリでは、Voice Isolator RTX 1.0.2のプラグイン本体をビルドできます。処理音と原音の割合は100%・80%・50%の3段階で、初期値は処理音100%です。

## ビルド

WindowsでGit、CMake 3.25以降、Visual Studio 2022の「C++によるデスクトップ開発」（MSVC v143とWindows SDK）を用意します。次のコマンドをPowerShellで実行してください。

```powershell
git clone https://github.com/sayaka-aiart/voice-isolator-rtx-vst3.git
cd voice-isolator-rtx-vst3
git submodule update --init --depth 1 third_party/vst3sdk
git -C third_party/vst3sdk submodule update --init --depth 1 base cmake pluginterfaces public.sdk
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target VoiceIsolatorRTX --parallel
```

生成先は `build/VST3/Release/VoiceIsolatorRTX.vst3/` です。DAWで使う際は、このフォルダーを丸ごとVST3の検索先に配置し、再スキャンしてください。

## 音声処理に必要なもの

ビルドはGPUのないWindows PCでも行えます。DAWで音を処理するには、対応するNVIDIA GPUと、Voice Isolator RTX 1.0.2用の推論エンジン（エンジン版1.0.0）が必要です。既に配布版を使っているPCでは、そのセットアップが登録した専用Python・CUDAライブラリ・モデルをそのまま使います。

プラグインは現在のユーザーのレジストリ `HKCU\Software\Local Audio Lab\Voice Isolator RTX` の `EngineRoot` からエンジンを起動します。登録済みのエンジンがない場合は「GPU engine」が「Unavailable」になり、エフェクト出力は無音になります。

## ライセンス

プラグイン独自部分は[MITライセンス](LICENSE)です。VST3 SDKはサブモジュールで固定したSteinberg製のMITライセンスのSDKです。VSTはSteinberg Media Technologies GmbHの商標です。[Steinberg VST3 SDK](https://github.com/steinbergmedia/vst3sdk)

音声分離にはInworld Voice Isolatorを使用します。ライセンスと著作権表示はVST3バンドルにも同梱されます。[Inworld Voice Isolator](https://github.com/inworld-ai/voice-isolator)・[Apache License 2.0](licenses/Inworld-LICENSE.txt)・[NOTICE](licenses/Inworld-NOTICE.txt)
