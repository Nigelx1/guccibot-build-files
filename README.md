# GucciBot build files

Files that go into every [GucciBot](https://github.com/Nigelx1/GucciBot) package but are too big to keep in its repository. GitHub refuses any file over 100 MB, and one of these is 106 MB.

You don't need anything from here to use GucciBot. Download it from [guccibot.net](https://guccibot.net) or [its releases](https://github.com/Nigelx1/GucciBot/releases); every package already has these files inside. This repository is where GucciBot's builds fetch them from.

## What's here

**Release [`ffmpeg-n8.0.1`](https://github.com/Nigelx1/guccibot-build-files/releases/tag/ffmpeg-n8.0.1)**
- **`ffmpeg-libs.zip`:** the seven FFmpeg DLLs that GucciBot's renderer loads on Windows to make videos:
  - `avcodec-62.dll`
  - `avdevice-62.dll`
  - `avfilter-11.dll`
  - `avformat-62.dll`
  - `avutil-60.dll`
  - `swresample-6.dll`
  - `swscale-9.dll`
- **`SHA256SUMS.txt`:** the SHA-256 of each DLL and of the zip.

## How GucciBot uses it

Every push to GucciBot builds one package for every platform: Windows, macOS, iOS and Android 32/64-bit. The workflow is [`.github/workflows/multi-platform.yml`](https://github.com/Nigelx1/GucciBot/blob/multiplatform/.github/workflows/multi-platform.yml). Each build job downloads `ffmpeg-libs.zip` from this release and checks it against the SHA-256 written in that workflow. It refuses to build if the file differs, then packs the DLLs in.

That makes the package GitHub builds from a commit complete, and it's the exact file a GucciBot release ships. There's one universal build: it's the same whether it comes from a commit, a release or the site.

## Where the DLLs come from

The files are a Windows x64 shared build of **FFmpeg n8.0.1-64-g15504610b0**, built 2026-02-23. It was configured with `--enable-gpl --enable-version3`, and the version string is embedded in each DLL. They're byte-identical to the copies [Silicate](https://git.puppy.lgbt/silicate/silicate) uses, whose renderer GucciBot's is built on.

FFmpeg is free software. This build is licensed under the [GNU GPL v3](https://www.gnu.org/licenses/gpl-3.0.html), and so is GucciBot. FFmpeg's source:
- [ffmpeg.org/download.html](https://ffmpeg.org/download.html)
- [github.com/FFmpeg/FFmpeg](https://github.com/FFmpeg/FFmpeg), release 8.0.1 at commit `15504610b0`

FFmpeg's legal page is [ffmpeg.org/legal.html](https://ffmpeg.org/legal.html).

## Updating

To change the DLLs:
1. Make a new release here with a new tag. Don't replace an existing release's files, because older GucciBot commits point at them.
2. Put the new tag and the zip's SHA-256 in GucciBot's `multi-platform.yml`.
