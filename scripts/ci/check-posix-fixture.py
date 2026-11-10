#!/usr/bin/env python3
"""Necessary host/permission prerequisites only; native CTests remain mandatory."""
import argparse
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--root', type=Path, required=True)
p.add_argument('--backend', type=Path, required=True)
p.add_argument('--report', type=Path, required=True)
a = p.parse_args()
if os.getuid() == 0 or os.getuid() != os.geteuid() or os.getgid() != os.getegid():
    raise RuntimeError('An ordinary real/effective host identity is required')
if not a.root.is_absolute() or not a.root.is_dir():
    raise RuntimeError('Explicit existing native fixture parent required')
with tempfile.TemporaryDirectory(prefix='ownership-preflight-', dir=a.root) as work:
    path = Path(work) / 'private-probe'
    fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        os.write(fd, b'ownership probe')
        os.fsync(fd)
        os.fchmod(fd, 0)
        info = os.fstat(fd)
        if info.st_uid != os.getuid() or stat.S_IMODE(info.st_mode) != 0:
            raise RuntimeError('Selected fixture does not preserve native ownership/mode')
        try:
            extra = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
        except PermissionError:
            pass
        else:
            os.close(extra)
            raise RuntimeError('Selected fixture does not enforce denied access for this caller')
    finally:
        os.fchmod(fd, 0o600)
        os.close(fd)
record = {'uid': os.getuid(), 'gid': os.getgid(), 'root': str(a.root),
          'ownership_mode_reopen_gate': 'PASS_PREREQUISITE_ONLY',
          'native_provider_qualification': 'NOT_ESTABLISHED_BY_THIS_PROBE'}
if sys.platform == 'linux':
    # Actual selected-root observation only, not a default provider claim or
    # substitute for native handle qualification and all registered tests.
    record['host_filesystem_type'] = subprocess.check_output(
        ['stat', '-f', '--printf=%T', '--', str(a.root)], text=True).strip()
    status = dict(line.split(':', 1) for line in Path('/proc/self/status').read_text().splitlines() if ':' in line)
    for field in ('CapEff', 'CapPrm', 'CapInh'):
        if int(status[field].strip(), 16):
            raise RuntimeError('Host runner must have zero capabilities: ' + field)
    record['host_capabilities'] = {field: status[field].strip() for field in ('CapEff', 'CapPrm', 'CapInh')}
    record['host_no_new_privs'] = status.get('NoNewPrivs', '').strip()
    # Both committed wrappers in 03a5e05 honor this SAME explicit mode and
    # reuse bootstrap_private_scope_namespace.py. This is only a bootstrap
    # prerequisite probe; the full CTest run still executes all six real retry
    # bind-alias cases. /bin/true here never counts as those native passes.
    mode = os.environ.get('CK_FS_NAMESPACE_BOOTSTRAP', '')
    if mode not in ('unprivileged', 'privileged-id-map'):
        raise RuntimeError('Explicit supported CK_FS_NAMESPACE_BOOTSTRAP required')
    for tool in ('unshare',):
        subprocess.run([tool, '--version'], check=True, stdout=subprocess.PIPE)
    subprocess.run([sys.executable, str(a.backend / 'tests/run_private_scope_mounts.py'),
                    '--probe', str(a.root)], check=True)
    record['shared_namespace_bootstrap_probe'] = 'PASS_PREREQUISITE_ONLY'
    record['retry_six_native_cases'] = 'REQUIRED_FULL_CTEST_NOT_QUALIFIED_BY_THIS_PROBE'
    record['private_scope_bootstrap'] = os.environ.get('CK_FS_NAMESPACE_BOOTSTRAP', 'unprivileged')
    # Do not put the whole CTest run under sudo, capabilities or no-new-privs:
    # the explicit committed bootstrap needs sudo to map its own isolated child.
    # Native ordinary-metadata mode (UID1000/cap0/NNP) is NOT registered CTest;
    # only --policy is registered. Do not invent that supplemental coverage.
a.report.write_text(json.dumps(record, indent=2) + '\n')
