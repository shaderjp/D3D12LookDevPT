# Asset Setup

D3D12LookDevPT does not store large scene assets in this repository. Test scenes, HDRIs, and high-resolution texture sets should be downloaded separately and placed beside the solution.

## Recommended Bistro Layout

For Bistro testing, download Amazon Lumberyard Bistro from NVIDIA ORCA:

https://developer.nvidia.com/orca/amazon-lumberyard-bistro

The NVIDIA page lists the asset as CC-BY 4.0 and provides the original FBX/Falcor files. After extracting the archive, place the folder next to the solution:

```text
D3D12LookDevPT/
  Bistro_v5_2/
    BistroExterior.fbx
    BistroInterior.fbx
    BistroInterior_Wine.fbx
    san_giuseppe_bridge_4k.hdr
    Textures/
```

`Bistro_v5_2/` is ignored by git and should remain local.

## Quick Launch

After building Debug x64, launch with a scene:

```powershell
.\Bin\x64\Debug\D3D12LookDevPT.exe --scene .\Bistro_v5_2\BistroExterior.fbx
```

Launch with an environment map:

```powershell
.\Bin\x64\Debug\D3D12LookDevPT.exe --scene .\Bistro_v5_2\BistroExterior.fbx --environment .\Bistro_v5_2\san_giuseppe_bridge_4k.hdr
```

You can also use `Project > Open Scene...` and `Project > Open Environment...` from the UI.

## Supported Scene And Texture Inputs

Scene import is handled by Assimp and is currently intended for static meshes:

- glTF / GLB
- FBX
- OBJ

Texture loading is handled by DirectXTex/WIC:

- PNG
- JPEG
- TGA
- DDS
- HDR

KTX/Basis-compressed texture workflows are not part of v1.

## Verification

Run the setup checker:

```powershell
.\Scripts\CheckSetup.ps1 -CheckAssets
```

Expected Bistro checks:

- `Bistro_v5_2/BistroExterior.fbx` exists
- `Bistro_v5_2/Textures/` exists
- optional interior FBX files and HDRI are present if you want to test them

## Notes

- Keep downloaded assets out of git.
- If textures do not resolve, keep the original extracted relative folder structure.
- FBX material conversion is best effort based on what Assimp reports.
- When publishing screenshots or derived work, follow the license and citation guidance from the asset source.
