[Return to the index](./00_index.md)
## Sound effects
The menu has the ability to play sound effects during navigation. It is not currently possible to customize them.

## Background music
The menu supports playing background music while browsing. When enabled, the music will loop continuously until you boot a game or disable the feature.

### Setup
1. Create an MP3 file named `bg.mp3`
2. Place it in the `/menu/` folder on your SD card (e.g., `sd:/menu/bg.mp3`)
3. Enable the "Background Music" option in the menu settings (requires BETA_SETTINGS to be enabled)

### Audio requirements
- **Format:** MP3 (MPEG-1 Audio Layer III)
- **Sample rate:** 44100 Hz recommended (44.1 kHz)
- **Channels:** Stereo or mono
- **Bitrate:** 128-192 kbps recommended for good balance of quality and size

### Creating a compatible MP3 file
You can use tools like [Audacity](https://www.audacityteam.org/) or [FFmpeg](https://ffmpeg.org/) to convert audio to the correct format.

**Using FFmpeg:**
```bash
ffmpeg -i input.wav -ar 44100 -ab 128k -ac 2 bg.mp3
```

**Using Audacity:**
1. Open your audio file in Audacity
2. Go to `File > Export > Export as MP3`
3. Set the following options:
   - Bit Rate Mode: Constant
   - Quality: 128 kbps or 192 kbps
   - Sample Rate: 44100 Hz
4. Save as `bg.mp3`

### Notes
- Background music automatically pauses when you use the Music Player to play a different MP3 file
- Background music resumes when you exit the Music Player
- Large MP3 files may take a moment to load on startup

## Music files
MP3 playback is supported, see [here](./41_mp3_player.md)