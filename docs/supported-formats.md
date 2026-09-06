# Supported Formats

Input is decoded by the user's FFmpeg, so anything your FFmpeg reads works.
Routinely exercised: WAV, MP3, FLAC, OGG, M4A, MP4, MKV, WebM, MOV.

- Container quirks: multi-stream files use the **first** audio stream;
  multichannel audio is mixed to mono.
- Outputs: PNG (own encoder, 8-bit RGBA) and MP4/WebM/MKV via FFmpeg
  (`libx264` default, `libx265`/`libvpx-vp9` selectable, CRF quality).
- Batch folder scan covers: wav mp3 flac ogg m4a aac wma mp4 mkv webm mov avi
  (override with `--ext`).
