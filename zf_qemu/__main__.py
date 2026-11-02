"""One portable entry point for CLI and browser sessions."""
import argparse
import json
from pathlib import Path
import signal
import subprocess
import time

from .runtime import Configuration
from .qmp import query as qmp


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('verify', 'run', 'web'))
    parser.add_argument('--config', type=Path, required=True, help='local JSON configuration')
    parser.add_argument('--qemu', type=Path, help='override QEMU executable path')
    parser.add_argument('--port', type=int, default=8766)
    parser.add_argument('--seconds', type=float, help='optional CLI duration; omitted means no time limit')
    parser.add_argument('--option', action='append', default=[], help='CLI machine KEY=VALUE override')
    args = parser.parse_args()
    if args.seconds is not None and args.seconds <= 0:
        parser.error('--seconds must be positive')
    try:
        config = Configuration.load(args.config, args.qemu)
        identities = config.validate()
        if args.mode == 'verify':
            print(json.dumps({'matched': identities}, indent=2))
            return
        from . import gui_server as gui
        gui.configure(config)
        if args.mode == 'web':
            gui.serve(args.port)
            return
        config.ensure_media()
        image = gui.TEST_INPUT
        if not image.exists():
            image.write_bytes(gui.make_test_chart_png())
        gui.image_to_nz9raw(image.read_bytes(), gui.TEST_RAW)
        run, process = config.launch(gui.TEST_RAW, args.option)
        print(f'Run directory: {run}', flush=True)
        print(f'Log: {run / "qemu.log"}', flush=True)
        def stop(*_):
            raise KeyboardInterrupt
        signal.signal(signal.SIGTERM, stop)
        deadline = time.monotonic() + 120
        try:
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError(f'QEMU exited ({process.returncode}); see {run / "qemu.log"}')
                try:
                    qmp(run, 'query-status')
                    break
                except (OSError, ValueError):
                    time.sleep(.2)
            else:
                raise TimeoutError('QMP was not ready within 120 seconds')
            # Virtual boot-position lever: preserve the tested external input sequence.
            qmp(run, 'qom-set', {'path': '/machine', 'property': 'mcu-pins', 'value': 'C5=1'})
            time.sleep(3)
            qmp(run, 'qom-set', {'path': '/machine', 'property': 'mcu-pins', 'value': 'C5=0'})
            print('QMP ready; running until stopped.' if args.seconds is None else 'QMP ready.', flush=True)
            try:
                code = process.wait(timeout=args.seconds)
            except KeyboardInterrupt:
                code = 0
            except subprocess.TimeoutExpired:
                code = 0
        finally:
            if process.poll() is None:
                try:
                    qmp(run, 'quit')
                except (OSError, ValueError):
                    process.terminate()
                try:
                    process.wait(10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        raise SystemExit(code)
    except (ValueError, OSError, RuntimeError, TimeoutError) as exc:
        parser.exit(1, f'{exc}\n')


if __name__ == '__main__':
    main()
