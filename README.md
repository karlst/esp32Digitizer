# ESP32 Digitizer

ESP32-based 24-bit data acquisition system built around an ADS1256 ADC.

Target architecture:

```text
ADS1256 -> ESP32-S3 -> ring buffer -> microSD
                              -> decimated/status data -> ESP32 Feather -> browser UI
```

## Repository Layout

```text
esp32Digitizer/
    s3Recorder/     ESP32-S3 acquisition/recorder firmware
    featherUi/      ESP32 Feather UI/control firmware
    shared/         Shared headers, protocol definitions, version constants
    backups/        Local firmware/reference backups
    docs/           Project documentation
```

## Development Environment

Primary development environment:

- Windows 11
- Visual Studio Code
- PlatformIO
- Arduino framework
- Git / GitHub

Both firmware projects are PlatformIO projects.

## Branches

- `main` - stable branch
- `develop` - active development branch

Normal workflow:

```text
edit -> build -> upload -> test -> commit -> push
```

## Building

Open either PlatformIO project in VS Code:

```text
s3Recorder/
featherUi/
```

Then use:

```text
PlatformIO -> Project Tasks -> General -> Build
```

## Uploading

Connect the appropriate ESP32 over USB, then use:

```text
PlatformIO -> Project Tasks -> General -> Upload
```

For serial output:

```text
PlatformIO -> Project Tasks -> General -> Monitor
```

Both projects currently use a serial monitor rate of:

```text
115200 baud
```

## Shared Code

Common code is stored under:

```text
shared/
```

For example:

```text
shared/version.hpp
```

Both PlatformIO projects include the shared directory through their `build_flags`.

## Current Version

```text
0.1.0
```
