"""Select opt-in hardware tests without putting diagnostic sources in src/.

PlatformIO runs this before compilation. Normal firmware compiles no test source;
Choke environments add the synthetic acquisition implementation, and standalone
diagnostics select one entry point through custom_test_source in platformio.ini.
This script builds only. It never opens a serial port or uploads firmware.
"""
Import("env")
from pathlib import Path

testRoot = Path(env.subst('$PROJECT_DIR')) / 'test' / 'hardware'
env.Append(CPPPATH=[str(testRoot), env.subst('$PROJECT_DIR/src'),
                   env.subst('$PROJECT_DIR/src/backend')])
# BuildSources captures its include paths before PlatformIO's library discovery.
# Test sources outside src/ therefore need the same explicitly declared library
# headers as the application; the libraries themselves are still linked by PIO.
frameworkRoot = env.PioPlatform().get_package_dir('framework-arduinoespressif32')
env.Append(CPPPATH=[env.subst('$PROJECT_DIR/lib/bufferedWriter/src'),
                   env.subst('$PROJECT_LIBDEPS_DIR/$PIOENV/SdFat/src'),
                   str(Path(frameworkRoot) / 'libraries' / 'SPI' / 'src')])
selectedSource = env.GetProjectOption('custom_test_source', '')
definitions = env.ParseFlags(env.get('BUILD_FLAGS', [])).get('CPPDEFINES', [])
chokeEnabled = any(item == 'S3_CHOKE_TEST' or
                   isinstance(item, (list, tuple)) and item[0] == 'S3_CHOKE_TEST'
                   for item in definitions)
if selectedSource:
    if selectedSource not in ['sdCardDiagnostic.cpp', 'sdInputDiagnostic.cpp',
                               'sdLedDiagnostic.cpp', 'sdWriteTiming.cpp']:
        raise ValueError('Unknown hardware test source: ' + selectedSource)
    env.BuildSources('$BUILD_DIR/hardwareTests', str(testRoot),
                     src_filter=['-<*>', '+<' + selectedSource + '>'])
elif chokeEnabled:
    env.BuildSources('$BUILD_DIR/hardwareTests', str(testRoot),
                     src_filter=['-<*>', '+<chokeTest.cpp>', '+<chokeEvents.cpp>',
                                 '+<acquisitionChoke.cpp>'])
