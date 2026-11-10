#!/usr/bin/env python3
"""Observe potential owners and retain CI roots; never infer deletion authority."""
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

work = Path(os.environ['CKTOOLS_CI_WORK_ROOT'])
source = Path(os.environ['CKTOOLS_CI_SOURCE_DIR'])
native_text = os.environ.get('CKTOOLS_CI_NATIVE_ROOT', '')
roots = [work, source] + ([Path(native_text)] if native_text else [])
expected = [('ckutilities-ci', work), ('source-', source)] + ([('ckutilities-native.', Path(native_text))] if native_text else [])
for prefix, root in expected:
    if not root.is_absolute() or not root.name.startswith(prefix) or root.is_symlink():
        raise RuntimeError('Unexpected CI root; retain and refuse ownership inference: ' + str(root))

issues = []
references = []
processes = []
keys = [str(p).replace(chr(92), '/').rstrip('/') for p in roots]

def observe(pid, kind, text, **extra):
    # Record only paths which may refer to this task. Unrelated process data is
    # inspected transiently, not copied wholesale into project artifacts.
    if any(key in text.replace(chr(92), '/') for key in keys):
        references.append(dict(pid=pid, kind=kind, observation=text, **extra))

def issue(pid, operation, error):
    issues.append(dict(pid=pid, operation=operation,
                       error_type=type(error).__name__, errno=getattr(error, 'errno', None)))

def run_inventory(command):
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode or result.stderr.strip():
        issues.append(dict(operation='inventory_command', executable=command[0],
                           exit=result.returncode, stderr=result.stderr))
    return result.stdout

if os.name == 'nt':
    # CIM command lines cannot establish native open HANDLE ownership. No
    # elevated helper, kernel enumeration workaround, or process termination.
    issues.append(dict(operation='Windows_open_handle_audit',
                       reason='UNSUPPORTED: no audited native open-handle inventory available'))
elif sys.platform == 'linux':
    def pids():
        return {int(x.name) for x in Path('/proc').iterdir() if x.name.isdecimal()}

    def identity(directory):
        # Field22 is the process start tick. A reused PID is not the same owner.
        fields = (directory / 'stat').read_text().rsplit(')', 1)[1].split()
        return fields[19]

    try:
        before = pids()
    except OSError as error:
        issue(None, 'enumerate_proc', error)
        before = set()
    for pid in sorted(before):
        directory = Path('/proc') / str(pid)
        info = dict(pid=pid)
        try:
            info['start_ticks_before'] = identity(directory)
        except (OSError, IndexError) as error:
            issue(pid, 'stat_before', error)
        for name in ('cwd', 'exe'):
            try:
                observe(pid, name, os.readlink(directory / name))
            except OSError as error:
                # Permission denied, disappearance, and empty kernel tasks all
                # leave an explicit gap; none are silently treated as no owner.
                issue(pid, name, error)
        try:
            text = (directory / 'cmdline').read_bytes().replace(b'\0', b' ').decode(errors='surrogateescape')
            observe(pid, 'cmdline', text)
        except OSError as error:
            issue(pid, 'cmdline', error)
        try:
            fds = sorted((directory / 'fd').iterdir(), key=lambda x: int(x.name))
            info['fd_count_observed'] = len(fds)
            for fd in fds:
                try:
                    observe(pid, 'open_fd', os.readlink(fd), fd=int(fd.name))
                except OSError as error:
                    issue(pid, 'read_fd_' + fd.name, error)
            if {x.name for x in (directory / 'fd').iterdir()} != {x.name for x in fds}:
                issues.append(dict(pid=pid, operation='fd_inventory', reason='changed_during_audit'))
        except OSError as error:
            issue(pid, 'enumerate_fd', error)
        try:
            data = (directory / 'maps').read_bytes()
            info['maps_sha256_observed'] = hashlib.sha256(data).hexdigest()
            info['maps_lines_observed'] = len(data.splitlines())
            for line in data.splitlines():
                fields = line.split(None, 5)
                if len(fields) == 6:
                    name = re.sub(rb'\\([0-7]{3})', lambda m: bytes([int(m[1], 8)]), fields[5])
                    observe(pid, 'mapped_file', os.fsdecode(name))
        except OSError as error:
            issue(pid, 'maps', error)
        try:
            info['start_ticks_after'] = identity(directory)
            if info.get('start_ticks_before') != info['start_ticks_after']:
                issues.append(dict(pid=pid, operation='process_identity', reason='changed_during_audit'))
        except (OSError, IndexError) as error:
            issue(pid, 'stat_after', error)
        processes.append(info)
    try:
        after = pids()
        if before != after:
            issues.append(dict(operation='process_inventory', reason='changed_during_audit',
                               appeared=sorted(after - before), disappeared=sorted(before - after)))
    except OSError as error:
        issue(None, 'enumerate_proc_after', error)
elif sys.platform == 'darwin':
    try:
        # No -u or -d restriction: include all observable processes' open FDs,
        # mapped/text files and cwd. Restricted owners/diagnostics retain roots.
        raw = run_inventory(['/usr/sbin/lsof', '-n', '-P', '-F0pftn'])
        pid = None
        fd = None
        file_type = None
        for field in raw.split('\0'):
            field = field.lstrip('\n')
            if field.startswith('p'):
                pid = int(field[1:])
                fd = file_type = None
                processes.append(dict(pid=pid))
            elif field.startswith('f'):
                fd = field[1:]
                file_type = None
            elif field.startswith('t'):
                file_type = field[1:]
            elif field.startswith('n') and pid is not None:
                observe(pid, 'lsof_open_or_mapped_file', field[1:], fd=fd, file_type=file_type)
        raw = run_inventory(['ps', '-ww', '-axo', 'pid=,command='])
        for line in raw.splitlines():
            fields = line.strip().split(None, 1)
            if len(fields) == 2:
                observe(int(fields[0]), 'cmdline', fields[1])
    except (OSError, ValueError) as error:
        issue(None, 'mac_process_inventory', error)
else:
    issues.append(dict(operation='ownership_inventory', reason='UNSUPPORTED platform'))

record = dict(status='RETAINED_UNQUALIFIED', time_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
              platform=sys.platform, roots=[str(p) for p in roots], helper_pid=os.getpid(),
              potential_references=references, inspected_processes=processes, incomplete_observations=issues,
              roots_deleted=False, processes_killed=False, privilege_changes=False,
              reason='Snapshots cannot establish complete stable ownership; automatic deletion is disabled',
              limitations=['Unreadable owners, permissions, PID/FD races or unsupported APIs never imply absence.',
                           'Path observations do not prove all native kernel references/aliases or block later opens.',
                           'Even an empty observable snapshot is not deletion authority; owner cleanup remains pending.'])
report_path = str(work / 'cleanup-audit.json')
if os.name == 'nt':
    if not report_path.replace('/', chr(92)).upper().startswith('V:' + chr(92) + 'TMP' + chr(92)):
        raise RuntimeError('Windows cleanup report must remain below V tmp; roots retained')
    report_path = chr(92) * 2 + '?' + chr(92) + report_path.replace('/', chr(92))
Path(report_path).write_text(json.dumps(record, indent=2) + '\n')
print(json.dumps(dict(status=record['status'], roots=record['roots'],
                      incomplete_observations=len(issues), potential_references=len(references),
                      report=report_path, roots_deleted=False)))
print('::warning::CI roots retained; cleanup ownership prerequisites remain unqualified. No process is killed.')
