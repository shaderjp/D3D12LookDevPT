# アセットの配置

D3D12LookDevPT では、大きな scene asset を repository に含めません。テスト用 scene、HDRI、高解像度 texture set は利用者側で別途ダウンロードし、solution の横に配置してください。

`images/` 以下の documentation screenshot には Bistro などの外部 test scene が写る場合がありますが、元の scene asset は local-only として扱い、この repository では配布しません。

## Bistro の推奨配置

Bistro を使う場合は、NVIDIA ORCA の Amazon Lumberyard Bistro を取得してください。

https://developer.nvidia.com/orca/amazon-lumberyard-bistro

NVIDIA のページでは CC-BY 4.0 として案内されており、オリジナルの FBX / Falcor file が提供されています。archive を展開したら、以下のように solution の横へ置きます。

```text
D3D12LookDevPT/
  Bistro_v5_2/
    BistroExterior.fbx
    BistroInterior.fbx
    BistroInterior_Wine.fbx
    san_giuseppe_bridge_4k.hdr
    Textures/
```

`Bistro_v5_2/` は `.gitignore` 済みです。local asset として扱い、git には入れないでください。

## 起動例

Debug x64 を build した後、scene を指定して起動できます。

```powershell
.\Bin\x64\Debug\D3D12LookDevPT.exe --scene .\Bistro_v5_2\BistroExterior.fbx
```

environment map も同時に指定できます。

```powershell
.\Bin\x64\Debug\D3D12LookDevPT.exe --scene .\Bistro_v5_2\BistroExterior.fbx --environment .\Bistro_v5_2\san_giuseppe_bridge_4k.hdr
```

UI から読み込む場合は `Project > Open Scene...` と `Project > Open Environment...` を使います。

## 対応 scene / texture

scene import は Assimp 経由で行います。v1 は static mesh 向けです。

- glTF / GLB
- FBX
- OBJ

texture loading は DirectXTex / WIC 経由です。

- PNG
- JPEG
- TGA
- DDS
- HDR

KTX / Basis 圧縮 texture workflow は v1 の対象外です。

## 確認方法

setup checker を実行します。

```powershell
.\Scripts\CheckSetup.ps1 -CheckAssets
```

Bistro については主に以下を確認します。

- `Bistro_v5_2/BistroExterior.fbx` が存在する
- `Bistro_v5_2/Textures/` が存在する
- interior FBX や HDRI は、必要に応じて追加確認する

## メモ

- ダウンロードした asset は git に入れないでください。
- texture が見つからない場合は、展開時の相対 folder 構造を保っているか確認してください。
- FBX material は Assimp から取得できる情報をもとに best effort で変換します。
- screenshot や派生成果を公開する場合は、asset 配布元の license / citation を確認してください。
