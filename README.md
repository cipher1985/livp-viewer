# LivpViewer

Fast single-file Windows viewer for Apple Live Photo (`.livp`) files.

## Download

See [Releases](https://github.com/cipher1985/livp-viewer/releases) for `LivpViewer.exe` (v1.0+).

## Features

- Open / drag `.livp` (ZIP: still JPEG/HEIC + short MOV)
- Click image (or LIVE icon) to play; click / Esc / Space to stop
- Mouse wheel zoom; right-drag to pan when zoomed
- ← → / PageUp PageDown or on-screen arrows to browse other `.livp` in the same folder
- Offers to install HEIF / HEVC Microsoft Store extensions when missing (`Ctrl+I`)

## Build (Visual Studio C++ x64)

```bat
cd viewer-native
build.bat
```

Output: `viewer-native\LivpViewer.exe`

## Optional: XnView MP still plugin

Shows the still frame inside `.livp` in XnView MP (no Live playback inside XnView).

```bat
cd plugin
build.bat
install.bat
```
## Notes

- **JPEG** stills: usually work with built-in Windows codecs
- **HEIC**: [HEIF Image Extensions](https://apps.microsoft.com/detail/9pmmsr1cgpwg)
- **HEVC** MOV: [HEVC Video Extensions](https://apps.microsoft.com/detail/9n4wgh0z6vhq)

## License

- Application source: MIT
- [miniz](https://github.com/richgel999/miniz): public domain / unlicense
