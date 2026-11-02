"""Shared CLI/WebUI configuration, immutable-input checks, and VM launch."""
from __future__ import annotations

from dataclasses import dataclass
from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import uuid

from .media import create_card

PACKAGE = Path(__file__).resolve().parent
EXPECTED = json.loads((PACKAGE / 'inputs.json').read_text())
LOCAL_IMAGES = frozenset({'snor_user', 'nand'})
REQUIRED = ('eg', 'loader', 'snor_user', 'li', 'mcu')
PROFILE = (
    'lens=on',
    'zf-lens-settings=50:100',
    'zf-ramdump-zeros=on', 'zf-multi-io-counter=on',
    'zf-sensorif-aux=on', 'zf-sensorif-output-config=on', 'zf-sensorif-group=on',
    'zf-sensorif-primary=on', 'zf-sensorif-source-period-ns=33333333',
    'zf-isp1a3-config=on', 'zf-isp199-config=on', 'zf-sensorif-source-stop=on',
    'zf-stream154c-empty=on', 'zf-isp192-config=on', 'zf-isp-small-config=on',
    'zf-statistics=on', 'zf-still=on',
)


def identity(path):
    path = Path(path).resolve()
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return {'path': str(path), 'bytes': path.stat().st_size, 'sha256': digest.hexdigest()}


def check_input(name, path):
    path = Path(path)
    if not path.is_file():
        raise ValueError(f'Missing {name} input: {path}')
    if name in LOCAL_IMAGES:
        # These are user-supplied state images, not fixed firmware releases.
        with path.open('rb') as stream:
            stream.read(1)
        return {'path': str(path.resolve()), 'validation': 'readable local image'}
    expected = EXPECTED[name]
    if path.stat().st_size != expected['bytes']:
        raise ValueError(f'{name}: expected {expected["bytes"]} bytes, got {path.stat().st_size}')
    actual = identity(path)
    if actual['sha256'] != expected['sha256']:
        raise ValueError(f'{name}: SHA256 mismatch; expected {expected["sha256"]}, got {actual["sha256"]}')
    return actual


@dataclass
class Configuration:
    inputs: dict[str, Path]
    qemu: Path
    qemu_img: Path
    state_dir: Path
    media_sources: dict[str, Path]

    @classmethod
    def load(cls, path, qemu=None):
        path = Path(path).expanduser().resolve()
        data = json.loads(path.read_text(encoding='utf-8'))
        def resolve(value):
            result = Path(value).expanduser()
            return (result if result.is_absolute() else path.parent / result).resolve()
        missing = set(REQUIRED) - set(data.get('inputs', {}))
        unknown = set(data.get('inputs', {})) - (set(EXPECTED) | LOCAL_IMAGES)
        if missing or unknown:
            raise ValueError(f'Input names: missing={sorted(missing)}, unknown={sorted(unknown)}')
        binary = resolve(qemu or data['qemu'])
        state = data.get('state_dir')
        default_state = Path(os.environ.get('XDG_STATE_HOME', Path.home() / '.local/state')) / 'zf-qemu'
        config = cls({name: resolve(value) for name, value in data['inputs'].items()},
                     binary, resolve(data['qemu_img']) if data.get('qemu_img') else binary.with_name('qemu-img'),
                     resolve(state) if state else default_state,
                     {name: resolve(value) for name, value in data.get('media', {}).items()})
        if set(config.media_sources) - {'sd', 'microsd'}:
            raise ValueError('media supports sd and microsd only')
        return config

    @property
    def media_dir(self):
        return self.state_dir / 'media'

    def validate(self):
        if os.name != 'posix':
            raise ValueError('Run the launcher with Linux Python (use WSL on Windows).')
        for executable in (self.qemu, self.qemu_img):
            if not executable.is_file() or not os.access(executable, os.X_OK):
                raise ValueError(f'Executable not found: {executable}; build QEMU first')
        return {name: check_input(name, path) for name, path in self.inputs.items()}

    def ensure_media(self):
        self.media_dir.mkdir(parents=True, exist_ok=True)
        for name in ('sd', 'microsd'):
            target = self.media_dir / (name + '.raw')
            if target.exists():
                continue
            if name in self.media_sources:
                temporary = target.with_suffix('.creating')
                subprocess.run([str(self.qemu_img), 'convert', '-f', 'raw', '-O', 'raw',
                                '-S', '4k', str(self.media_sources[name]), str(temporary)], check=True)
                temporary.replace(target)
            else:
                create_card(target)

    def new_run(self):
        stamp = datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S') + '-' + uuid.uuid4().hex[:6]
        run = self.state_dir / 'runs' / stamp
        if len(str(run / 'qmp.sock').encode()) >= 104:
            raise ValueError('state_dir is too long for Unix sockets; choose a shorter Linux path')
        run.mkdir(parents=True, exist_ok=False)
        return run

    @contextmanager
    def idle_guard(self):
        """One VM or media writer may own a state directory at a time."""
        import fcntl
        self.state_dir.mkdir(parents=True, exist_ok=True)
        with (self.state_dir / 'session.lock').open('a+b') as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                raise RuntimeError('A VM is already using this state_dir; stop it or choose a separate state_dir') from None
            yield lock

    def command(self, run, identities, sensor_input, options=()):
        """Build both frontends' command, always including the validated LI."""
        for path in (*self.inputs.values(), run, sensor_input):
            if any(char in str(path) for char in ',\n\r'):
                raise ValueError('Input and state paths cannot contain commas or newlines')
        machine = ['nikon-zf', 'loader=' + str(self.inputs['loader']),
                   'snor-user=' + str(self.inputs['snor_user']), 'image-in-dram=off',
                   'li-image=' + str(self.inputs['li']), 'cpu3-entry=0xa07800000', 'li-wait-ipc=off',
                   'mcu-image=' + str(self.inputs['mcu']), 'battery-standin=on']
        if 'nand' in self.inputs:
            shutil.copyfile(self.inputs['nand'], run / 'nand-working.img')
            machine.append('nand=' + str(run / 'nand-working.img'))
        extra = {}
        for option in options:
            if '=' not in option or any(char in option for char in ',\n\r'):
                raise ValueError('Machine option must be KEY=VALUE without commas or newlines')
            key, value = option.split('=', 1)
            if key in {'loader', 'snor-user', 'li-image', 'mcu-image', 'nand'}:
                raise ValueError('Firmware paths must come from validated inputs')
            extra[key] = value
        profile = dict(option.split('=', 1) for option in PROFILE)
        profile['z9-capture-raw'] = str(sensor_input)
        profile.update(extra)
        machine += [key + '=' + value for key, value in profile.items()]
        drives = []
        for name in ('sd', 'microsd'):
            overlay = run / (name + '-working.qcow2')
            subprocess.run([str(self.qemu_img), 'create', '-q', '-f', 'qcow2', '-F', 'raw',
                            '-b', str(self.media_dir / (name + '.raw')), str(overlay)], check=True)
            machine.append(f'zf-{name}-drive=zf-{name}')
            drives += ['-drive', f'if=none,id=zf-{name},format=qcow2,file={overlay}']
        command = [str(self.qemu), '-machine', ','.join(machine), '-cpu', 'cortex-a53',
                   '-smp', '4,maxcpus=5', '-bios', str(self.inputs['eg']),
                   '-serial', 'file:' + str(run / 'serial.log'), '-monitor', 'none',
                   '-display', 'none', '-nic', 'none', '-no-reboot', '-no-shutdown',
                   '-qmp', f'unix:{run}/qmp.sock,server=on,wait=off',
                   '-gdb', f'unix:{run}/gdb.sock,server=on,wait=off', *drives]
        manifest = {'inputs': identities, 'command': command,
                    'profile': 'functional Zf display/capture; synthetic battery, lens and sensor',
                    'camera_operations': 0}
        (run / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        return command

    def launch(self, sensor_input, options=()):
        identities = self.validate()
        with self.idle_guard() as lock:
            self.ensure_media()
            run = self.new_run()
            command = self.command(run, identities, sensor_input, options)
            with (run / 'qemu.log').open('ab') as output:
                process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT,
                                           pass_fds=(lock.fileno(),))
        return run, process
