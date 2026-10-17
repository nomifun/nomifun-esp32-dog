# Native BoardDog button and volume tests

Run from the repository root with any Python 3:

```powershell
python tests/dog_host/board_buttons_host/run.py
```

The runner extracts the current `ChangeVolume`, `InitializeVolumeTimers`,
`MusicSwitchTimerCb` and `InitializeButtons` methods from `board_dog.cc` and
compiles them with the installed Windows MSVC toolchain. The JSON receipt and
compilation/test logs are written under `build/dog_host/board_buttons_host`.

The host test separates ESP timer callback context from the queued application
context. Display access, media actions, motion and volume writes reject calls in
the timer context. It exercises delayed dispatch, release before either level of
long-press work, queued repeats after release or leaving the volume page,
volume clamps, menu double-click behavior, the captured two-key servo gesture,
BOOT menu exit before media/conversation work, cancellation of playback still
pending behind TTS without toggling chat, and missing-display behavior.

This checks application dispatch and stale callback handling. The substitutes do
not access GPIO, ESP timers, the OLED, NVS, a real player, the robot, USB or the
network. Electrical debounce, device timing and physical key behavior require
separate hardware acceptance.
