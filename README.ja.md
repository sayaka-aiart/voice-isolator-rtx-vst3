# Voice Isolator RTX

[English](README.md) | **日本語**

話し声の背景ノイズをリアルタイムに抑えるWindows x64用VST3プラグインです。このリポジトリでは、Voice Isolator RTX 1.0.2のプラグイン本体をビルドできます。処理音と原音の割合は100%・80%・50%の3段階で、初期値は処理音100%です。

## 遅延

プラグインは自身の遅延をホストに申告し、ホストがそれを補正します。48 kHz・128サンプルのバッファーでは2,176サンプル（45.3 ms）です。**Low latency** をオンにすると2,080サンプル（43.3 ms）になります。256サンプルのバッファーでは4.7 ms、512サンプルでは10 ms短くなります。どちらの設定でも、GPUが1フレームを処理する時間として10 ms以上を確保します。標準設定はそこにバッファー1つ分の余裕を足しています。ほかのアプリがGPUを使っていて「Processed audio」が100%を下回る場合は、**Low latency** をオフにしてください。設定を切り替えるとホストが遅延補正を計算し直すため、音声が数秒途切れることがあります。

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

## 推論モデルのダウンロード

初めて使うPCでは、DAWを閉じてWindows配布版の `VoiceIsolatorRTX-1.0.2-Setup-x64.exe` を実行してください。セットアップが推論エンジンを導入し、モデルのダウンロードと検証も行います。プラグインで **GPU engine → Ready** と表示されていれば、モデルは導入済みです。

Voice Isolator RTX 1.0.2が使うのは、Inworldのリアルタイムモデル `denoiser_realtime.safetensors`（232,780,800バイト、約233 MB）です。モデルを単独で取得するには、次のリンクから空のフォルダーに保存するか、そのフォルダーでPowerShellを開いて下のコマンドを実行してください。[モデル本体をダウンロード（Hugging Face）](https://huggingface.co/inworld/voice-isolator/resolve/9b0285b3fe8403aa8e2f1dbb2165a80a4294c970/denoiser_realtime.safetensors)

```powershell
curl.exe --fail --location --retry 2 --output denoiser_realtime.safetensors "https://huggingface.co/inworld/voice-isolator/resolve/9b0285b3fe8403aa8e2f1dbb2165a80a4294c970/denoiser_realtime.safetensors"
if ($LASTEXITCODE -ne 0) { throw 'Model download failed.' }
```

どちらの方法で取得した場合も、保存先のフォルダーで次を実行してください。`True` と表示されればSHA-256が一致しています。`False` の場合は、使用する前にダウンロードし直してください。

```powershell
$expected = '76598f05523c0669e76562a8b2f31c9d395fca316e80c907ee11775e7497ae0f'
(Get-FileHash -LiteralPath '.\denoiser_realtime.safetensors' -Algorithm SHA256 -ErrorAction Stop).Hash -eq $expected
```

導入済みの環境でモデルが消えた場合や破損した場合は、DAWを閉じてください。64ビット版PowerShellで次を実行すると、プラグインが読み込むファイルの場所が表示されます。検証済みのファイルをその場所にコピーし、プラグインを開き直して **GPU engine** が **Ready** になることを確認してください。

```powershell
$key = 'HKCU:\Software\Local Audio Lab\Voice Isolator RTX'
$engine = Get-ItemPropertyValue -LiteralPath $key -Name EngineRoot -ErrorAction Stop
Join-Path $engine 'inworld\models\denoiser_realtime.safetensors'
```

## ライセンス

プラグイン独自部分は[MITライセンス](LICENSE)です。VST3 SDKはサブモジュールで固定したSteinberg製のMITライセンスのSDKです。VSTはSteinberg Media Technologies GmbHの商標です。[Steinberg VST3 SDK](https://github.com/steinbergmedia/vst3sdk)

音声分離にはInworld Voice Isolatorを使用します。ライセンスと著作権表示はVST3バンドルにも同梱されます。[Inworld Voice Isolator](https://github.com/inworld-ai/voice-isolator)・[Apache License 2.0](licenses/Inworld-LICENSE.txt)・[NOTICE](licenses/Inworld-NOTICE.txt)
