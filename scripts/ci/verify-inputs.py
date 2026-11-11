#!/usr/bin/env python3
"""Bind a CI build to its actual frontend and immutable dependency checkouts."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source', type=Path, required=True)
p.add_argument('--vision', type=Path, required=True)
p.add_argument('--backend', type=Path, required=True)
p.add_argument('--report', type=Path, required=True)
a = p.parse_args()
def native_io(path):
    if os.name != 'nt':
        return path
    text = os.fspath(path).replace('/', chr(92))
    if not text.startswith(chr(92) * 2 + '?' + chr(92)):
        text = (chr(92) * 2 + '?'+chr(92)+'UNC'+chr(92)+text[2:]
                if text.startswith(chr(92) * 2) else chr(92) * 2 + '?' + chr(92) + text)
    return Path(text)

pins = json.loads(native_io(a.source / 'scripts/ci/dependency-pins.json').read_text())
for key in ('backend', 'ckvision'):
    if not re.fullmatch('[0-9a-f]{40}', pins[key]):
        raise ValueError('An immutable full dependency commit is required: ' + key)

def git(root, *args):
    return subprocess.check_output(['git', '-C', str(root), *args], text=True).strip()

expected_frontend = os.environ['CKTOOLS_CI_FRONTEND_REVISION']
if not re.fullmatch('[0-9a-f]{40}', expected_frontend):
    raise ValueError('The triggering immutable frontend SHA is required')
observed = {'frontend': git(a.source, 'rev-parse', 'HEAD'),
            'ckvision': git(a.vision, 'rev-parse', 'HEAD'),
            'backend': git(a.backend, 'rev-parse', 'HEAD')}
if observed != dict(frontend=expected_frontend, ckvision=pins['ckvision'], backend=pins['backend']):
    raise RuntimeError('Actual source/dependency commits differ from immutable pins')
if pins['backend'] != os.environ['CKTOOLS_CI_BACKEND_REVISION'] or pins['ckvision'] != os.environ['CKTOOLS_CI_CKVISION_REVISION']:
    raise RuntimeError('Workflow and source dependency pin records disagree')
for root in (a.vision, a.backend):
    if git(root, 'status', '--porcelain=v1', '--untracked-files=all'):
        raise RuntimeError('Dependency checkout is not pristine: ' + str(root))
# Full tag history is fetched by checkout. Release assets must come from a tag
# whose exact commit is on master, not from an unrelated tag/branch tip.
tag = os.environ.get('CKTOOLS_CI_RELEASE_TAG', '')
if tag:
    if not re.fullmatch(r'v[0-9]+(?:\.[0-9]+){2}', tag):
        raise ValueError('Release requires a numeric vMAJOR.MINOR.PATCH tag')
    if git(a.source, 'rev-parse', '--verify', 'refs/tags/' + tag + '^{commit}') != expected_frontend:
        raise RuntimeError('Release tag and triggering source commit differ')
    subprocess.run(['git', '-C', str(a.source), 'merge-base', '--is-ancestor',
                    expected_frontend, 'refs/remotes/origin/master'], check=True)
    version = re.search(r'set\(CKTOOLS_VERSION\s+"([0-9.]+)"', native_io(a.source / 'CMakeLists.txt').read_text())
    if not version or version.group(1) != tag[1:]:
        raise RuntimeError('Release tag does not match the actual pinned frontend product version')
# Hash tracked source/assets only; nested dependency worktrees are separate.
inputs = {}
input_modes = {}
for name, root in [('frontend', a.source), ('ckvision', a.vision), ('backend', a.backend)]:
    paths = subprocess.check_output(['git', '-C', str(root), 'ls-files', '--stage', '-z']).split(b'\0')
    files = {}
    modes = {}
    for raw in paths:
        if raw:
            metadata, filename = raw.split(b'\t', 1)
            relative = os.fsdecode(filename)
            modes[relative] = metadata.split()[0].decode('ascii')
            if metadata.startswith(b'160000 '):
                files[relative] = {'gitlink': metadata.split()[1].decode('ascii')}
                continue
            path = native_io(root / relative)
            if path.is_symlink():
                data = os.fsencode(os.readlink(path))
            else:
                data = path.read_bytes()
            files[relative] = hashlib.sha256(data).hexdigest()
    inputs[name] = files
    input_modes[name] = modes
record = {'revisions': observed, 'release_tag': tag, 'source_asset_sha256': inputs,
          'source_asset_git_mode': input_modes,
          'parallel_builds': 1, 'parallel_tests': 1,
          'baseline': 'No comparable clean runner baseline established; inspect actual compiler/input/output records.'}
native_io(a.report).write_text(json.dumps(record, indent=2) + '\n')
print(json.dumps(observed))
