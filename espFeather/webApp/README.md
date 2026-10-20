# Feather web application

This is the preserved browser interface for the older S3 recorder, DAC controls,
and LED test. It does not control the P4. The current Feather terminal build does
not compile this project.

`src` contains implementations, `include` contains headers, and `data` contains
browser files. `platformio.ini` builds the `featheresp32` environment when this
folder is explicitly selected as the project. See [build instructions](../../docs/BUILDING.md).
Browser files need a separate LittleFS filesystem upload when changed.

The LED implementation stays in `../test/hardware` and is linked by
`buildWebApp.py`. PC monitor checks and browser checks stay in `../test` and use
these real application files. There are no duplicate implementations.
