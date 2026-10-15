The ES module wrapper of ffmpeg.wasm (`@ffmpeg/ffmpeg` 0.12.15, `dist/esm`, unchanged), MIT license,
https://github.com/ffmpegwasm/ffmpeg.wasm. It is kept here because the browser starts its worker only from the page's own origin.
The FFmpeg core itself (`@ffmpeg/core` 0.12.10, about 31 MB, FFmpeg with libx264, GPL) is fetched from a CDN when the page converts.
