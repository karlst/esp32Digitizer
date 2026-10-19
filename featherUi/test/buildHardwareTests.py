"""Build only the opt-in ADC diagnostic entry point; never upload or open USB.

The normal source filter selects the reusable ADC class; this adds the test's
setup/loop outside src, keeping the ordinary web/DAC application out of the build.
"""
Import("env")
from pathlib import Path

projectRoot = Path(env.subst('$PROJECT_DIR'))
frameworkRoot = Path(env.PioPlatform().get_package_dir('framework-arduinoespressif32'))
env.Append(CPPPATH=[str(projectRoot / 'src' / 'adc'),
                   str(projectRoot.parent / 'shared'),
                   str(frameworkRoot / 'libraries' / 'SPI' / 'src')])
selectedSource = env.GetProjectOption('custom_test_source', 'ads127SpiDiagnostic.cpp')
if selectedSource not in ['ads127SpiDiagnostic.cpp', 'ads127CaptureControl.cpp', 'benchDacLoopback.cpp']:
    raise ValueError('Unknown Feather hardware test: ' + selectedSource)
env.BuildSources('$BUILD_DIR/hardwareTests', str(projectRoot / 'test' / 'hardware'),
                 src_filter=['-<*>', '+<' + selectedSource + '>', '+<benchDac.cpp>', '+<inputSnapshot.cpp>'])
