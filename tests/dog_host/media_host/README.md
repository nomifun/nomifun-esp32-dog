# Media behavior verification

Run from the repository root:

```powershell
node tests/dog_host/media_host/run.mjs
```

The runner uses the installed MSVC toolchain and compiles the actual `dog_media.cc`.
Only the SD music directory string is replaced with a new directory under
`build/dog_host/media_host`; device data is never opened. Clock, FreeRTOS
notifications, NVS, MP3 decoder output, application state, and the audio service
are deterministic host substitutes. Both translation units expose private members
in the host compile so tests can execute the actual worker for one iteration.

The behavior cases cover missing versus empty SD directories; a ten-second TTS
deferral followed by one alarm playback; temporary audio ownership and decoder
failures; the existing sixty-second alarm grace window; one in-flight request;
replacement of a pending alarm by a normal music request; explicit stop;
failed NVS commits followed by persistence recovery without replay;
immediate stopped state and callback consistency; and compact alarm fields.

The playback-toggle cases execute the actual Worker after it has dequeued a
request and entered a TTS wait. A second toggle must cancel that waiting request
before any PCM, while a toggle after stop must replay the remembered valid song.
They also cover first-PCM stop, replacement by a newer generation, callback
reentry, continuous intent through automatic track advance, a removed current
file, and both command orders of real concurrent Stop/Toggle threads. The suite
currently reports nineteen behavior cases.

Build output, assertions, source SHA-256 values, and limitations are saved to
`build/dog_host/media_host/{build.log,test.log,result.json}`. This proves
application state and retry behavior. It does not prove the real decoder, MP3
sample-rate combinations, FatFS errors, physical I2S sound, ADC accuracy, or ESP32
timing. Device acceptance must separately exercise real songs, concurrent TTS,
clock synchronization, a missing/removed SD card, and audible stop/track changes.
