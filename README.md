# LivpViewer / XnView LIVP

Windows tools for Apple Live Photo (`.livp`) files.

## LivpViewer (recommended)

Fast single-file native viewer (~200KB):

- Open / drag `.livp`
- Click image (or LIVE icon) to play embedded MOV
- Click / Esc / Space to stop
- Mouse wheel zoom; right-drag to pan when zoomed
- ← → / PageUp PageDown or on-screen arrows to browse other `.livp` in the same folder
- Offers to install HEIF / HEVC Store extensions when missing

### Build

Requires Visual Studio with C++ x64 tools:

```bat
cd viewer-native
build.bat
```

Output: `viewer-native\LivpViewer.exe`

### Download

See [Releases](../../releases) for prebuilt `LivpViewer.exe`.

## XnView MP still-image plugin

Shows the still frame inside `.livp` in XnView MP (no Live playback in XnView).

```bat
cd plugin
build.bat
install.bat
```

Installs `XLivp.usr` into `XnViewMP\Plugins` (may need Administrator).

## Notes

- `.livp` = ZIP containing still (JPEG/HEIC) + short MOV
- HEIC needs [HEIF Image Extensions](https://apps.microsoft.com/detail/9pmmsr1cgpwg)
- HEVC MOV needs [HEVC Video Extensions](https://apps.microsoft.com/detail/9n4wgh0z6vhq)

## License

miniz is public domain. Application code is provided as-is for personal use.
