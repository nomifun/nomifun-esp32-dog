# Native Windows PetDog behavior tests

From the repository root, run:

```powershell
node tests/dog_host/pet_dog_host/run.mjs
```

The runner uses the existing MSVC environment and the installed ESP-IDF cJSON
sources (`IDF_PATH` must be set). It does not start WSL, change network
settings, download dependencies, open USB, flash firmware or move a real robot.
`build/dog_host/pet_dog_host/result.json` records the result and exact PetDog
source hashes.

The production PetDog source is compiled with only its SD path redirected into
the test directory. Windows-only test adapters supply POSIX directory operations;
FreeRTOS, LEDC and NVS are host substitutes. Coverage includes calibration and
right-rear trim, action cancellation/replacement, pending-command visibility,
both ordering cases and 100 concurrent explicit/automatic command pairs, held
sit/stand/lie/stop postures, aliases, movement after a held posture, and explicit
free-explore opt-in. The action worker is paused at its test entry to exercise
the queue-to-worker gap deterministically.

This establishes host command arbitration and PWM calculations. Server tool
invocation, production ESP32 timing and physical walking still require separate
device acceptance.
