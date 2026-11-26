# Voice Isolator RTX

[English](README.md) | **日本語**

話し声の背景ノイズをリアルタイムに抑えるWindows x64用VST3プラグインです。このリポジトリからプラグイン本体をビルドできます。

1.3.0では **Dual mono** を追加しました。バイノーラルマイクを使うときは、左右の入力を1本のステレオトラックに割り当て、Voice Isolator RTXを1つ挿して **Dual mono** をオンにしてください。左右を個別にノイズ除去します。オフ（初期値）では左右の平均を処理し、同じ処理音を左右に返します。再生中に切り替えるとバイパス中でも一瞬音が途切れるため、録音・配信前に設定してください。設定はプロジェクトに保存されます。

**Extra synthesis advance** の **5 ms** または **10 ms** ボタンを押すと、48 kHzでDAWに報告するプラグインの遅延がその分だけ短くなります。初期値の **0 ms** は1.1.0と同じタイミングで音を合成します。

**Noise reduction** では処理音と原音の割合を選べます。100%（初期値）は処理音だけを出し、80%・50%ではそれぞれ原音を20%・50%混ぜます。

## 遅延モード

3つの設定とも同じモデルで10 ms刻みに推論します。5 ms・10 msでは合成時に前後の波形を重ねる時間を短くし、処理音を早く出力します。素材によって声の質感や短い物音の残り方が変わることがあります。

**Low latency** がオフのStandard（初期値）では、GPUの処理時間を10 ms確保します。オンにすると9 msになり、どの合成モードでも48 kHzでさらに1 ms短縮できます。表は **48 kHz・最大ブロック長128サンプル** でDAWに申告するプラグイン遅延です。

| 合成の前倒し | 用途の目安 | Standard | Low latency |
|---|---|---:|---:|
| 0 ms（初期値） | 1.1.0と同じ音で録音・ミックスしたいとき。 | 45.33 ms | 44.33 ms |
| 5 ms | 自分の声をモニターする際に、前倒しを中程度に抑えて遅延を短くしたいとき。 | 40.33 ms | 39.33 ms |
| 10 ms | モニターの遅延を最優先したいとき。声の質感は0 msと聴き比べられます。 | 35.33 ms | 34.33 ms |

可変バッファーに対応するため、どのモードでも最大ブロック1個分を確保します。48 kHzでは最大256サンプルなら表の値に約2.67 ms、最大512サンプルなら8 msが加わります。マイクからヘッドホンまでの遅延には、機器とドライバーの往復分も加わります。

GPUに負荷がかかり「Processed audio」が100%を下回る場合は、Standardに切り替えてください。どちらの遅延設定を変更しても、DAWに遅延補正の更新を通知します。切り替え直後は、新しい設定で出力がそろうまで音が一瞬途切れることがあります。

1.3.0より前のプロジェクトは **Dual mono** がオフで開きます。1.2.0より前のプロジェクトは前倒し **0 ms** で開き、保存済みの除去の強さ、バイパス、遅延モードを引き継ぎます。Low latency導入前の設定にはStandardを使います。**Dual mono** がオフの場合、0 msで保存した設定は1.1.0以降、5 ms・10 msは1.2.0以降で開けます。**Dual mono** をオンにして保存した設定の復元には1.3.0以降が必要です。

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

ビルドはGPUのないWindows PCでも行えます。DAWで音を処理するには、対応するNVIDIA GPUとVoice Isolator RTXの推論エンジン1.0.0が必要です。既に配布版を使っているPCでは、そのセットアップが登録した専用Python・CUDAライブラリ・モデルをそのまま使います。

プラグインは現在のユーザーのレジストリ `HKCU\Software\Local Audio Lab\Voice Isolator RTX` の `EngineRoot` からエンジンを起動します。登録済みのエンジンがない場合は「GPU engine」が「Unavailable」になり、エフェクト出力は無音になります。

## 推論モデルのダウンロード

`VoiceIsolatorRTX-1.3.0-Windows-x64.zip` を展開し、DAWを閉じて `VoiceIsolatorRTX-1.3.0-Setup-x64.exe` を実行してください。初回はセットアップがVST3と推論エンジン1.0.0を導入し、モデルをダウンロードして検証します。同じ場所に更新する場合は、検証済みのエンジンとモデルを再利用します。このリポジトリで自分でビルドしたVST3を使う場合は、インストール先のVST3フォルダーをビルドしたフォルダーごと置き換えてからDAWを起動してください。

Voice Isolator RTXが使うのは、Inworldのリアルタイムモデル `denoiser_realtime.safetensors`（232,780,800バイト、約233 MB）です。モデルを単独で取得するには、次のリンクから空のフォルダーに保存するか、そのフォルダーでPowerShellを開いて下のコマンドを実行してください。[モデル本体をダウンロード（Hugging Face）](https://huggingface.co/inworld/voice-isolator/resolve/9b0285b3fe8403aa8e2f1dbb2165a80a4294c970/denoiser_realtime.safetensors)

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
