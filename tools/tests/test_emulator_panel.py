#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""The synthetic Voyage panel image the emulator gets, and the rules it must pass.

`tools/emulator/make_voyage_panel.py` is what makes the QEMU machine show a
Voyage panel at all: a 512 KiB mx25l4005a NOR image carrying a WBF at 0x886 and
the panel information page at 0x70000, the 2048-byte WRF waveform the stock
driver derives from it, and the PW3WFP1 envelope readom's device-data path
builds. The stock 5.13.6 display stack, the 0700 kernel patch and
`boot/devicedata/devicedata.c` each accept only a narrow shape, and the
generator carries replicas of all three so it can prove its own output passes
before QEMU sees it.

This test runs the generator into a temporary directory (never the
`readom-port/emu-voyage` copy), runs its `--verify-only` path, and checks the
files it wrote: determinism, the exact sizes, the WBF header fields and CRC,
the decoded panel information, and that each consumer replica accepts the
result. A corrupt edit must be refused rather than accepted.

Nothing here needs QEMU, network, or a running guest.
"""
import gzip
import hashlib
import importlib.util
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GENERATOR = ROOT / 'tools/emulator/make_voyage_panel.py'
OUTPUTS = ('panel.img', 'synth.wrf.gz', 'synth.pw3.gz')

# What the running guest reports for this synthetic panel (readom-port's
# emu-voyage environment report and tools/emulator/README.md): the generator is
# only useful if the driver decodes these.
EXPECTED_BCD = 'EBA'
EXPECTED_VCOM = '-2.05'
EXPECTED_XWI = 'V220_EBAL_ED060TC1_3CE'
EXPECTED_WRF_HUMAN = 'V220_EBAL_3CE'

checks = 0
failures = []


def check(ok, what):
    global checks
    checks += 1
    if not ok:
        failures.append(what)


def load():
    spec = importlib.util.spec_from_file_location('pw3_make_voyage_panel', GENERATOR)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def generate(module, directory):
    return module.main(['--out', str(directory), '--quiet'])


def digests(directory):
    return {name: hashlib.sha256((directory / name).read_bytes()).hexdigest()
            for name in OUTPUTS}


def format_cases(module):
    check(module.PANEL_FLASH_SIZE == 0x80000, 'the panel flash is not 512 KiB')
    check(module.DEFAULT_WFM_ADDR == 0x886, 'the WBF is not at 0x886')
    check(module.PNL_INFO_ADDR == 0x70000, 'the panel information page is not at 0x70000')
    check(module.WBF_FILE_LENGTH == 0x10000, 'the WBF file length is not 65536')
    check(module.WRF_SIZE == 2048, 'the WRF is not 2048 bytes')
    check(module.XWI_STRING == b'V220_EBAL_ED060TC1_3CE',
          'the panel XWI string changed: %r' % module.XWI_STRING)


def generated_cases(module, work):
    first, second = work / 'first', work / 'second'
    check(generate(module, first) == 0, 'the generator did not return success')
    check(generate(module, second) == 0, 'the generator did not return success on a second run')
    for name in OUTPUTS:
        check((first / name).is_file(), 'the generator did not write %s' % name)
    if not all((first / name).is_file() for name in OUTPUTS):
        return
    check(digests(first) == digests(second),
          'two runs of the generator produced different files: %r vs %r'
          % (digests(first), digests(second)))

    panel = (first / 'panel.img').read_bytes()
    check(len(panel) == module.PANEL_FLASH_SIZE,
          'panel.img is %d bytes, not 512 KiB' % len(panel))
    wbf = panel[module.DEFAULT_WFM_ADDR:module.DEFAULT_WFM_ADDR + module.WBF_FILE_LENGTH]
    check(len(wbf) == module.WBF_FILE_LENGTH, 'the WBF is not 65536 bytes: %d' % len(wbf))
    header = {
        'file_length': module.get32(wbf, 4),
        'serial': module.get32(wbf, 8),
        'run_type': wbf[12],
        'fpl_platform': wbf[13],
        'mode_version': wbf[16],
        'wf_version': wbf[17],
        'wf_type': wbf[19],
        'panel_size': wbf[20],
        'amepd_part_number': wbf[21],
        'xwia': module.get32(wbf, 28) & 0xFFFFFF,
        'wmta': module.get32(wbf, 32) & 0xFFFFFF,
        'fvsn': wbf[35],
        'luts': wbf[36],
        'mc': wbf[37],
        'trc': wbf[38],
        'advanced_wfm_flags': wbf[39],
        'eb': wbf[40],
        'sb': wbf[41],
    }
    expected = {
        'file_length': 0x10000, 'serial': 0xBEEF, 'run_type': 0, 'fpl_platform': 3,
        'mode_version': 7, 'wf_version': 0, 'wf_type': 0x4B, 'panel_size': 60,
        'amepd_part_number': 1, 'xwia': 0x200, 'wmta': 58, 'fvsn': 1, 'luts': 0,
        'mc': 6, 'trc': 1, 'advanced_wfm_flags': 0, 'eb': 0xFF, 'sb': 0xFC,
    }
    for key, value in expected.items():
        check(header[key] == value, 'WBF header %s is 0x%x, not 0x%x'
              % (key, header[key], value))
    check(module.wbf_header_crc(wbf) == module.get32(wbf, 0),
          'the WBF checksum field is not the CRC-32 over the image')

    # The panel information page, decoded the way the stock driver decodes it.
    decoded = module.panel_driver_checks(panel)
    check(decoded['bcd'] == EXPECTED_BCD,
          'the panel BCD decodes to %r, not %r' % (decoded['bcd'], EXPECTED_BCD))
    check(decoded['vcom'] == EXPECTED_VCOM,
          'the panel VCOM decodes to %r, not %r' % (decoded['vcom'], EXPECTED_VCOM))
    check(decoded['xwi'] == EXPECTED_XWI,
          'the panel XWI decodes to %r, not %r' % (decoded['xwi'], EXPECTED_XWI))
    check('ED060TC1' in decoded['panel_id'],
          'the decoded panel id does not name the ED060TC1 panel: %r' % decoded['panel_id'])

    # The WRF: the 2048 bytes the stock driver loads, written as the stored
    # gzip member readom's dd_gzip_stored() emits.
    wrf_gz = (first / 'synth.wrf.gz').read_bytes()
    wrf = gzip.decompress(wrf_gz)
    check(len(wrf) == module.WRF_SIZE, 'synth.wrf.gz decompresses to %d bytes' % len(wrf))
    check(wrf[:48] == wbf[:48], 'the WRF does not begin with the WBF header')
    check(module.gzip_stored(wrf) == wrf_gz,
          'synth.wrf.gz is not the stored gzip member the generator writes')
    check(module.stock_process_fw_checks(wrf, wrf_gz) == module.WRF_XWI,
          'the stock waveform-version decode does not accept the WRF')

    # The envelope: PW3WFP1 with this panel's WBF, this WRF and no VC data;
    # readom's dd_envelope()/dd_gzip_stored() and the 0700 patch must accept it.
    pw3_gz = (first / 'synth.pw3.gz').read_bytes()
    envelope = module.pw3_gz_decompress(pw3_gz)
    check(envelope[:8] == b'PW3WFP1\x00',
          'synth.pw3.gz is not a PW3WFP1 envelope: %r' % envelope[:8])
    check(module.get32(envelope, 8) == 1, 'the envelope version is not 1')
    check(module.get32(envelope, 12) == 64, 'the envelope header is not 64 bytes')
    env_wbf, env_wrf, (vc_offset, vc_length) = module.android_wf_container(envelope)
    check(env_wbf == wbf, 'the envelope does not carry the generated WBF')
    check(env_wrf == wrf, 'the envelope does not carry the generated WRF')
    check(vc_offset == 0 and vc_length == 0,
          'an envelope with advanced=0 carries VC data (%d/%d)' % (vc_offset, vc_length))

    summary = module.verify(panel, wbf, wrf, wrf_gz, pw3_gz)
    check(summary['wrf_human_version'] == EXPECTED_WRF_HUMAN,
          'the WRF human version is %r, not %r'
          % (summary['wrf_human_version'], EXPECTED_WRF_HUMAN))

    check(module.main(['--out', str(first), '--verify-only', '--quiet']) == 0,
          '--verify-only refused the files the generator had just written')


def refusal_cases(module, work):
    """A damaged file must be refused by the same rules, not accepted."""
    source = work / 'source'
    generate(module, source)

    def variant(name):
        directory = work / name
        directory.mkdir()
        for output in OUTPUTS:
            (directory / output).write_bytes((source / output).read_bytes())
        return directory

    corrupted = variant('corrupted')
    panel = bytearray((corrupted / 'panel.img').read_bytes())
    panel[module.DEFAULT_WFM_ADDR + 100] ^= 0xFF
    (corrupted / 'panel.img').write_bytes(panel)
    try:
        module.main(['--out', str(corrupted), '--verify-only', '--quiet'])
    except Exception as failure:  # CheckError; a crash is also a refusal
        check(True, 'a corrupted WBF payload is refused (%s)' % type(failure).__name__)
    else:
        check(False, 'a corrupted WBF payload was accepted by --verify-only')

    truncated = variant('truncated')
    (truncated / 'panel.img').write_bytes((truncated / 'panel.img').read_bytes()[:-4096])
    try:
        module.main(['--out', str(truncated), '--verify-only', '--quiet'])
    except Exception as failure:
        check(True, 'a panel image of the wrong size is refused (%s)' % type(failure).__name__)
    else:
        check(False, 'a panel image of the wrong size was accepted')

    bad_envelope = variant('bad-envelope')
    envelope = bytearray(module.pw3_gz_decompress((bad_envelope / 'synth.pw3.gz').read_bytes()))
    envelope[8] = 2   # envelope version
    (bad_envelope / 'synth.pw3.gz').write_bytes(module.gzip_stored(envelope))
    try:
        module.main(['--out', str(bad_envelope), '--verify-only', '--quiet'])
    except Exception as failure:
        check(True, 'an envelope with the wrong version is refused (%s)'
              % type(failure).__name__)
    else:
        check(False, 'an envelope with the wrong version was accepted')


def main():
    check(GENERATOR.is_file(), '%s is missing' % GENERATOR.name)
    if not GENERATOR.is_file():
        return report()
    module = load()
    format_cases(module)
    with tempfile.TemporaryDirectory(prefix='pw3-emulator-panel-test-') as temporary:
        work = Path(temporary)
        generated_cases(module, work)
        refusal_cases(module, work)
    return report()


def report():
    print('%d checks, %d failed' % (checks, len(failures)))
    for failure in failures:
        print('  FAIL %s' % failure)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
