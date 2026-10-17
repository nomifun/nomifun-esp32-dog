# Native BoardDog menu callback tests

Run only this narrow suite from the repository root:

```powershell
python tests/dog_host/board_menu_host/run.py
```

The runner extracts the actual `SetMenuCallbacks` registration from
`board_dog.cc`, including both lambda bodies, and the actual `SetChatActive`,
`Schedule` and `SetListeningMode` methods from `application.cc`. No production
method body is copied into the test.

The test checks queued service routing, explicit/idempotent chat entry, the
execution-time Wi-Fi setup guard, delegation to `TogglePlayback`, exact music
playing/pending/stopped labels, error priority and the `/music/mp3` directory.
Media and transport/audio/state effects are substitutes. Real media Worker
semantics, display back navigation and the full chat API suite have separate
acceptance; this runner does not repeat them.

Results are written under `build/dog_host/board_menu_host`. No user
configuration, serial port, network, device or package installation is used.
