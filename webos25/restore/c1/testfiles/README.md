# C1 test files

- `dtsc1-check.mkv` — the DTS track of `../../testfiles/DTS-in-mp4.mp4` remuxed to
  MKV (`ffmpeg -i DTS-in-mp4.mp4 -map 0:a -c copy dtsc1-check.mkv`). Passthrough
  output must have sha256 `5cdce24f2e4ca0cc72253786e40094e7cd9d5f23b6f6b2c42658d006bf4c3c33`
  (= `ffmpeg -i dtsc1-check.mkv -map 0:a -c copy -f spdif - | sha256sum`).
- `dtsc1-listen.mkv` (not committed, 22 MB) — 60 s for listening tests:
  ```
  ffmpeg -stream_loop 15 -i ../../testfiles/DTS-HD-MA-5.1.ts -map 0:a:0 -c copy -f dts loop.dts
  ffmpeg -f lavfi -i color=c=black:s=1280x720:r=25 -i loop.dts -map 0:v -map 1:a -shortest \
    -c:v libx264 -preset veryfast -tune stillimage -pix_fmt yuv420p -c:a copy dtsc1-listen.mkv
  ```
  Expected passthrough sha256: `218fa6eed5b6b7ad2afd4977fa349d4e8bcd09f5aaa4d2cc8fa03660a569b74c`.
