# Feather firmware

The current terminal firmware is built from this folder. Its default is the
`ads127-p4-constant` test used for constant-input measurements. Other hardware
tests remain selectable by their environment names in `test/platformio.ini`.

- `src/adc` and `include/adc`: reusable ADS127 ADC driver.
- `test`: terminal firmware entry points, hardware diagnostics, and PC checks.
- `webApp`: separate web-interface project with its own `src`, `include`, `data`,
  and PlatformIO configuration. It is excluded from the parent build.

See [build instructions](../docs/BUILDING.md) for commands. Building compiles on
the PC; uploading and running on a board are separate steps.
