#pragma once
#include "stubs/fake.h"
#include "../pet_dog_host/win32/compat.h"
// Compile both translation units with the same access specifiers (MSVC encodes
// them in member symbol names) while exercising the production worker directly.
#define private public
#include "dog_media.h"
#undef private
// The clock is a hardware substitute, so rollover and retry behavior can be
// exercised deterministically without waiting or changing the system clock.
#define time media_host_time
