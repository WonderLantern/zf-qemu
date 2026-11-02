"""Loopback browser GUI for an interactive, indefinite Zf QEMU session."""
from __future__ import annotations
import argparse, base64, json, os, socket, struct, subprocess, threading, time, zlib
from email import policy
from email.parser import BytesParser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse

from .sd_upload import add_file as add_sd_file
from . import retro_dial_positions as retro
from .runtime import Configuration
from .qmp import query as qmp_query

ROOT = Path(__file__).resolve().parent
CONFIG = None
GUI_MEDIA = USER_SD = USER_MICROSD = TEST_INPUT = TEST_RAW = None
# Zf active image raster established by native 6048x4032 captures.
SENSOR_WIDTH, SENSOR_HEIGHT = 6048, 4032

def image_to_nz9raw(payload, target=None):
    """Convert every picture to a fixed 24.4 MP RGGB sensor-input frame."""
    target = Path(target or TEST_RAW)
    from array import array
    from io import BytesIO
    import sys
    from PIL import Image, ImageOps
    image = ImageOps.exif_transpose(Image.open(BytesIO(payload))).convert('RGB')
    # Fit without stretching or cropping; source resolution never changes
    # sensor geometry. Small inputs are upscaled, large inputs downsampled.
    image = ImageOps.contain(image, (SENSOR_WIDTH, SENSOR_HEIGHT), Image.Resampling.LANCZOS)
    canvas = Image.new('RGB', (SENSOR_WIDTH, SENSOR_HEIGHT), 'black')
    canvas.paste(image, ((SENSOR_WIDTH - image.width) // 2,
                        (SENSOR_HEIGHT - image.height) // 2))
    black, white = 512, 16383
    levels = [black + round((white - black) * value / 255) for value in range(256)]
    rgb = canvas.tobytes()
    temporary = target.with_name(target.name + '.tmp')
    with temporary.open('wb') as output:
        output.write(b'NZ9RAW1\0' + struct.pack('<6I', SENSOR_WIDTH, SENSOR_HEIGHT,
                                               14, 0, black, white))
        # One native CFA sample per sensor coordinate: RG on even rows,
        # GB on odd rows. Row writes bound temporary conversion memory.
        for y in range(SENSOR_HEIGHT):
            row = rgb[y * SENSOR_WIDTH * 3:(y + 1) * SENSOR_WIDTH * 3]
            even, odd = (0, 1) if not y & 1 else (1, 2)
            samples = array('H', [0]) * SENSOR_WIDTH
            samples[0::2] = array('H', map(levels.__getitem__, row[even::6]))
            samples[1::2] = array('H', map(levels.__getitem__, row[3 + odd::6]))
            if sys.byteorder != 'little':
                samples.byteswap()
            output.write(samples.tobytes())
    temporary.replace(target)
    return target

def make_test_chart_png(width=1024, height=682):
    """Create the default color-bar/checkerboard diagnostic image."""
    try:
        from PIL import Image, ImageDraw
        image = Image.new('RGB', (width, height), 'black')
        draw = ImageDraw.Draw(image)
        colors = [(255, 255, 255), (255, 255, 0), (0, 255, 255),
                  (0, 255, 0), (255, 0, 255), (255, 0, 0),
                  (0, 0, 255), (0, 0, 0)]
        bar_h = height * 2 // 3
        bar_w = max(1, width // len(colors))
        for index, color in enumerate(colors):
            draw.rectangle((index * bar_w, 0, (index + 1) * bar_w, bar_h), fill=color)
        cell = max(8, min(width, height) // 18)
        for y in range(bar_h, height, cell):
            for x in range(0, width, cell):
                if ((x // cell) + (y // cell)) % 2 == 0:
                    draw.rectangle((x, y, min(width - 1, x + cell - 1), min(height - 1, y + cell - 1)), fill='white')
        from io import BytesIO
        stream = BytesIO(); image.save(stream, format='PNG', optimize=True)
        return stream.getvalue()
    except Exception:
        return b''

class Session:
    def __init__(self):
        self.lock = threading.RLock()
        self.proc = None
        self.run = None
        self.capture_thread = None
        self.stop_capture = threading.Event()
        self.properties = set()
        self.last_state_read = 0.0
        self.cache = {'status': 'not-started', 'lcd': None, 'evf': None, 'toplcd': None, 'updated': 0}
        self.lifecycle = "not-started"
        self.start_error = None
        self.log_path = None
        self.fn_held = False
        self.fn_locked = False

    def _ensure_media(self):
        CONFIG.ensure_media()
        if not TEST_INPUT.exists() or TEST_INPUT.stat().st_size == 0:
            TEST_INPUT.write_bytes(make_test_chart_png(1024, 682))
        # Rebuild from the retained PNG so old low-resolution fixtures are
        # upgraded on the next start without requiring another upload.
        image_to_nz9raw(TEST_INPUT.read_bytes(), TEST_RAW)

    def test_input(self):
        self._ensure_media()
        return TEST_INPUT.read_bytes()

    def upload_test_input(self, filename, payload):
        if not payload:
            raise ValueError('测试图片为空')
        if len(payload) > 25 * 1024 * 1024:
            raise ValueError('测试图片不能超过 25 MB')
        # Decode and re-encode through Pillow when available so the browser
        # always receives a safe PNG regardless of the local source format.
        try:
            from io import BytesIO
            from PIL import Image, ImageOps
            image = ImageOps.exif_transpose(Image.open(BytesIO(payload))).convert('RGB')
            image.save(TEST_INPUT, format='PNG', optimize=True)
            image_to_nz9raw(TEST_INPUT.read_bytes(), TEST_RAW)
        except Exception as exc:
            raise ValueError(f'无法读取图片：{exc}') from exc
        restarted = False
        with self.lock:
            running = bool(self.proc and self.proc.poll() is None)
        if running:
            # The sensor source is opened by QEMU at startup.  Restart the
            # session after replacing the raw fixture so the new image is
            # consumed immediately instead of leaving the old stream active.
            self.stop()
            self.start()
            restarted = True
        width, height = struct.unpack('<2I', TEST_RAW.read_bytes()[8:16])
        return {'name': filename, 'bytes': TEST_INPUT.stat().st_size,
                'width': width, 'height': height,
                'model_rgb_width': width // 2, 'model_rgb_height': height // 2,
                'bayer_width': width, 'bayer_height': height,
                'path': str(TEST_INPUT), 'restarted': restarted}

    def reset_test_input(self, restart=True):
        CONFIG.ensure_media()
        TEST_INPUT.write_bytes(make_test_chart_png(1024, 682))
        image_to_nz9raw(TEST_INPUT.read_bytes(), TEST_RAW)
        restarted = False
        with self.lock:
            running = bool(self.proc and self.proc.poll() is None)
        if restart and running:
            # The sensor source is opened once at QEMU start, so a running
            # session keeps the previous stream until it is restarted.
            self.stop()
            self.start()
            restarted = True
        width, height = struct.unpack('<2I', TEST_RAW.read_bytes()[8:16])
        return {'name': '默认彩色条 + 黑白棋盘格', 'bytes': TEST_INPUT.stat().st_size,
                'width': width, 'height': height,
                'model_rgb_width': width // 2, 'model_rgb_height': height // 2,
                'path': str(TEST_INPUT), 'restarted': restarted}

    def qmp(self, name, args=None):
        return qmp_query(self.run, name, args)

    def start(self):
        self.lifecycle = "starting"
        self.start_error = None
        with self.lock:
            if self.proc and self.proc.poll() is None:
                try:
                    status = self.qmp('query-status')
                    if status.get('running'):
                        self.lifecycle = 'running'
                        return {'run': str(self.run), 'status': 'running'}
                except Exception:
                    pass
                self.proc.poll()
            self._ensure_media()
            self.run, self.proc = CONFIG.launch(TEST_RAW)
            self.log_path = self.run / 'qemu.log'
            deadline = time.monotonic() + 90
            last_error = None
            while time.monotonic() < deadline:
                if self.proc.poll() is not None:
                    detail = self._startup_log()
                    raise RuntimeError(f'WSL/QEMU exited with code {self.proc.returncode}: {detail or "no startup output"}')
                try:
                    status = self.qmp('query-status')
                    if status.get('running'):
                        for level in ('1', '0'):
                            self.qmp('qom-set', {'path': '/machine', 'property': 'mcu-pins', 'value': 'C5=' + level})
                            if level == '1': time.sleep(3)
                        self.properties = {x['name'] for x in (self.qmp('qom-list', {'path': '/machine'}) or [])}
                        self.stop_capture.clear()
                        self.last_state_read = 0
                        self.capture_thread = threading.Thread(target=self._capture_loop, daemon=True)
                        self.capture_thread.start()
                        self.lifecycle = 'running'
                        return {'run': str(self.run), 'status': 'running', 'qmp': 'ready'}
                except Exception as exc:
                    last_error = exc
                time.sleep(.2)
            self.proc.terminate()
            self.proc.wait(10)
            detail = self._startup_log()
            raise TimeoutError(f'QMP did not become ready: {last_error!r}; {detail or "no startup output"}')

    def upload(self, target, filename, payload):
        with self.lock:
            if self.proc and self.proc.poll() is None:
                raise RuntimeError('请先停止 QEMU 会话，再写入模拟卡')
            self._ensure_media()
            image = {'sd': USER_SD, 'microsd': USER_MICROSD}.get(target)
            if image is None:
                raise ValueError('目标卡必须是 SD 或 microSD')
            with CONFIG.idle_guard():
                result = add_sd_file(image, filename, payload)
            result.update({'target': target, 'image': str(image)})
            return result

    def _startup_log(self):
        if not self.log_path:
            return ''
        try:
            return tail(self.log_path, 5000).strip()
        except OSError:
            return ''

    def snapshot(self):
        # Publish transitions without waiting for the QMP/startup lock. Cache
        # dictionaries are replaced atomically by the capture worker.
        snapshot = {'run': str(self.run) if self.run else None, **self.cache}
        if self.lifecycle in {'starting', 'stopping', 'failed'}:
            snapshot['status'] = self.lifecycle
            if self.start_error:
                snapshot['error'] = self.start_error
        elif not self.proc or self.proc.poll() is not None:
            snapshot['status'] = 'stopped' if self.run else 'not-started'
        if snapshot['status'] in ('starting', 'stopping', 'stopped', 'not-started', 'failed'):
            snapshot.update(state={}, lcd=None, evf=None, native_lcd=None,
                            native_evf=None, toplcd=None)
        return snapshot

    def logs(self):
        chunks = []
        if self.log_path:
            try: chunks.append(tail(self.log_path, 12000))
            except OSError: pass
        if self.run:
            try: chunks.append(tail(self.run / 'gui-inputs.jsonl', 6000))
            except OSError: pass
        return '\n'.join(x for x in chunks if x).strip() or '尚无运行日志'

    def stop(self):
        self.lifecycle = "stopping"
        with self.lock:
            self.stop_capture.set()
            if self.proc and self.proc.poll() is None:
                try: self.qmp('quit')
                except Exception: self.proc.terminate()
                try: self.proc.wait(5)
                except subprocess.TimeoutExpired: self.proc.kill()
            self.cache = {'status': 'stopped', 'state': {}, 'lcd': None, 'evf': None,
                          'native_lcd': None, 'native_evf': None, 'test_input': False,
                          'toplcd': None, 'updated': time.time()}
            self.lifecycle = "stopped"
            self.start_error = None
            self.fn_held = False
            self.fn_locked = False
        if self.capture_thread and self.capture_thread is not threading.current_thread():
            self.capture_thread.join(5)

    def _set_button(self, value):
        self.qmp('qom-set', {'path': '/machine', 'property': 'mcu-button', 'value': value})

    def set_fn(self, phase):
        """Keep Fn active across other pointer actions; lock is a UI-level hold."""
        if not self.proc or self.proc.poll() is not None:
            raise RuntimeError('QEMU is not running')
        with self.lock:
            if phase == 'down':
                self.qmp('qom-set', {'path': '/machine', 'property': 'mcu-fn', 'value': True})
                self.fn_held = True
            elif phase == 'up':
                self.fn_locked = False
                self.qmp('qom-set', {'path': '/machine', 'property': 'mcu-fn', 'value': False})
                self.fn_held = False
            elif phase == 'lock':
                self.qmp('qom-set', {'path': '/machine', 'property': 'mcu-fn', 'value': True})
                self.fn_held = True
                self.fn_locked = True
            elif phase == 'unlock':
                self.fn_locked = False
                self.qmp('qom-set', {'path': '/machine', 'property': 'mcu-fn', 'value': False})
                self.fn_held = False
            else:
                raise ValueError('invalid Fn phase')
            with (self.run / 'gui-inputs.jsonl').open('a', encoding='utf-8') as log:
                log.write(json.dumps({'time': time.time(), 'key': 'fn', 'phase': phase}) + '\n')
            return {'accepted': True, 'key': 'fn', 'phase': phase, 'held': self.fn_held, 'locked': self.fn_locked}

    def _read_state(self):
        names = {x['name'] for x in (self.qmp('qom-list', {'path': '/machine'}) or [])}
        result = {}
        for prop in ('zf-panel-state', 'zf-toplcd-state', 'mcu-navigation', 'mcu-button', 'mcu-fn', 'mcu-menu',
                     'mcu-mode-adc', 'mcu-mode-lever', 'mcu-rotary', 'eye-near',
                     'zf-eye-state', 'zf-aperture-display', 'zf-access-led-state',
                     'zf-charge-lamp-state', 'zf-charge-led-state', 'zf-lens-state', 'zf-retro-dial-profile',
                     'zf-sd-capability', 'zf-microsd-capability'):
            if prop in names:
                try: result[prop] = self.qmp('qom-get', {'path': '/machine', 'property': prop})
                except Exception as exc: result[prop] = {'error': repr(exc)}
        if isinstance(result.get('zf-retro-dial-profile'), str):
            try: result['zf-retro-dial-profile-parsed'] = json.loads(result['zf-retro-dial-profile'])
            except (TypeError, ValueError): pass
        result['retro-dial-codes'] = self._retro_codes()
        if 'mcu-dial' in names:
            result['mcu-dial'] = '可用（写入式输入）'
        result['status'] = self.qmp('query-status')
        panel_value = result.get('zf-panel-state', '{}')
        panel = json.loads(panel_value) if isinstance(panel_value, str) else (panel_value or {})
        result['lcd_summary'] = panel.get('lcd', {})
        result['evf_summary'] = panel.get('evf', {})
        top_value = result.get('zf-toplcd-state')
        if isinstance(top_value, str):
            try:
                result['zf-toplcd-state-parsed'] = json.loads(top_value)
            except (TypeError, ValueError):
                result['zf-toplcd-state-parsed'] = {'text': top_value}
        elif isinstance(top_value, dict):
            result['zf-toplcd-state-parsed'] = top_value
        top = result.get('zf-toplcd-state-parsed')
        if isinstance(top, dict):
            text = top.get('text')
            if isinstance(text, str) and text.strip() and text.strip() != '--':
                result['zf-toplcd-text'] = text.strip()
            else:
                digits = top.get('digits')
                if isinstance(digits, (list, tuple)) and len(digits) >= 2:
                    glyphs = {0x3f: '0', 0x06: '1', 0x5b: '2', 0x4f: '3', 0x66: '4',
                              0x6d: '5', 0x7d: '6', 0x07: '7', 0x7f: '8', 0x6f: '9'}
                    left = glyphs.get(int(digits[0]))
                    right = glyphs.get(int(digits[1]))
                    if left is not None and right is not None:
                        result['zf-toplcd-text'] = left + ('.' if top.get('decimal') else '') + right
        return result, names

    def control(self, body):
        if not self.proc or self.proc.poll() is not None:
            raise RuntimeError('QEMU is not running')
        kind = body.get('kind')
        with self.lock:
            if kind == 'fn':
                phase = body.get('phase')
                if phase not in {'down', 'up', 'lock', 'unlock'}:
                    raise ValueError('unsupported Fn phase')
                return self.set_fn(phase)
            if kind == 'eye':
                prop, value = 'eye-near', bool(body.get('value'))
            elif kind == 'mode':
                prop, value = 'mcu-mode-adc', {'M': '0', 'A': '950', 'S': '1837', 'P': '2739', 'AUTO': '3600'}.get(str(body.get('value')).upper())
                if value is None: raise ValueError('unsupported exposure mode')
            elif kind == 'lever':
                prop, value = 'mcu-mode-lever', {'photo': 'idle', 'video': 'pos1', 'monochrome': 'pos2'}.get(body.get('value'))
                if value is None: raise ValueError('unsupported mode lever')
            elif kind == 'dial':
                axis = body.get('axis')
                delta = int(body.get('delta', 0))
                if axis not in {'main', 'sub'} or not 1 <= abs(delta) <= 127: raise ValueError('invalid dial detent')
                prop, value = 'mcu-dial', f'dial-{axis}={delta:+d}'
            elif kind == 'retro':
                row = int(body.get('row'))
                if row not in {0, 2, 5}: raise ValueError('invalid rotary row')
                info = None
                if body.get('position') is not None:
                    info = retro.describe(row, body.get('position'))
                    if info['mask'] is None: raise ValueError(info['warning'])
                    value = f'row{row}={info["mask"]}'
                else:
                    mask = body.get('mask')
                    if mask == 'open': value = f'row{row}=open'
                    else:
                        mask = int(mask)
                        if mask < 0 or mask > (127 if row == 2 else 63): raise ValueError('invalid rotary mask')
                        value = f'row{row}={mask}'
                prop = 'mcu-rotary'
            else:
                raise ValueError('unsupported control')
            if prop not in self.properties:
                self.properties = {x['name'] for x in (self.qmp('qom-list', {'path': '/machine'}) or [])}
            if prop not in self.properties: raise RuntimeError(f'model does not provide {prop}')
            self.qmp('qom-set', {'path': '/machine', 'property': prop, 'value': value})
            if kind in {'mode', 'eye', 'lever'}:
                actual = self.qmp('qom-get', {'path': '/machine', 'property': prop})
                matches = (bool(actual) == bool(value)) if kind == 'eye' else (str(actual) == str(value))
                if not matches:
                    raise RuntimeError(f'exposure mode write did not stick: requested {value}, read {actual}')
            with (self.run / 'gui-inputs.jsonl').open('a', encoding='utf-8') as log:
                log.write(json.dumps({'time': time.time(), 'kind': kind, 'property': prop, 'value': value}) + '\n')
            response = {'accepted': True, 'kind': kind, 'property': prop, 'value': value}
            if kind in {'mode', 'eye', 'lever'}: response['readback'] = actual
            if kind == 'retro':
                response['readback'] = self.qmp('qom-get', {'path': '/machine', 'property': 'mcu-rotary'})
                response['codes'] = self._retro_codes()
                if info is not None:
                    response['position'] = info
            return response

    def _retro_codes(self):
        """Read the three position codes the EX put into the 124 B report.

        The codes are what the original firmware derived from the contact mask
        at offsets 0x59 row0, 0x5a row2, 0x5b row5.
        """
        try:
            reply = self.qmp('human-monitor-command',
                             {'cpu-index': 4, 'command-line': 'x /3bx 0x20011379'})
        except Exception as exc:
            return {'error': repr(exc)}
        if not isinstance(reply, str):
            return {'error': 'unexpected monitor reply'}
        values = []
        for word in reply.replace(chr(13), ' ').replace(chr(10), ' ').split():
            if word.startswith('0x'):
                try:
                    values.append(int(word, 16))
                except ValueError:
                    pass
        if len(values) < 3:
            return {'error': 'short monitor reply', 'reply': reply}
        return {'row0': values[0], 'row2': values[1], 'row5': values[2]}

    def _capture_loop(self):
        while not self.stop_capture.is_set() and self.proc and self.proc.poll() is None:
            began = time.monotonic()
            try:
                with self.lock:
                    now = time.monotonic()
                    if now - self.last_state_read >= 1.0 or not self.cache.get('state'):
                        state, names = self._read_state()
                        self.last_state_read = now
                    else:
                        state = self.cache.get('state', {})
                        names = self.properties
                    prefix = self.run / 'gui-frame'
                    lcd_path = prefix.with_name(prefix.name + '.lcd.ppm')
                    evf_path = prefix.with_name(prefix.name + '.evf.ppm')
                    for path in (lcd_path, evf_path, prefix.with_name(prefix.name + '.json')):
                        try: path.unlink()
                        except FileNotFoundError: pass
                    if 'zf-panel-snapshot' in names:
                        self.qmp('qom-set', {'path': '/machine', 'property': 'zf-panel-snapshot', 'value': str(prefix)})
                    lcd = ppm_png(lcd_path) if lcd_path.exists() else b''
                    evf = ppm_png(evf_path) if evf_path.exists() else b''
                    if not lcd:
                        ppm = self.run / 'gui-screen.ppm'
                        self.qmp('screendump', {'filename': str(ppm)})
                        lcd = ppm_png(ppm) if ppm.exists() else b''
                    with self.lock:
                        self.cache = {'status': state.get('status', {}), 'state': state,
                                      'lcd': b64(lcd), 'evf': b64(evf),
                                      'native_lcd': b64(lcd), 'native_evf': b64(evf),
                                      'test_input': False,
                                      'toplcd': state.get('zf-toplcd-text') or state.get('zf-toplcd-state'), 'updated': time.time()}
            except Exception as exc:
                with self.lock: self.cache = {'status': 'capture-error', 'error': repr(exc), 'updated': time.time()}
            time.sleep(max(0, 1 / 3 - (time.monotonic() - began)))

    def press(self, key, repeat=1, interval=.26):
        allowed = {'menu', 'up', 'down', 'left', 'right', 'ok', 'play', 'delete', 'ae-af-lock', 'i', 'zoom-in', 'zoom-out', 'disp', 'movie', 'display-mode', 'fn', 'half-shutter', 'shutter'}
        if key not in allowed: raise ValueError('unsupported key')
        if not self.proc or self.proc.poll() is not None: raise RuntimeError('QEMU is not running')
        with self.lock:
            for index in range(max(1, min(int(repeat), 200))):
                if key == 'fn':
                    # Legacy /api/input callers still get a momentary Fn,
                    # but never through the ordinary mcu-button property.
                    self.qmp('qom-set', {'path': '/machine', 'property': 'mcu-fn', 'value': True})
                    time.sleep(.07)
                    self.qmp('qom-set', {'path': '/machine', 'property': 'mcu-fn', 'value': False})
                    with (self.run / 'gui-inputs.jsonl').open('a', encoding='utf-8') as log:
                        log.write(json.dumps({'time': time.time(), 'key': key, 'repeat_index': index + 1}) + '\n')
                    if index + 1 < repeat: time.sleep(max(.03, min(float(interval), 2)))
                    continue
                if key in {'half-shutter', 'shutter'}:
                    self.qmp('qom-set', {'path': '/machine', 'property': 'mcu-pins', 'value': 'G1=0'})
                    if key == 'shutter':
                        time.sleep(.08); self.qmp('qom-set', {'path': '/machine', 'property': 'mcu-pins', 'value': 'G13=0'})
                    time.sleep(.08)
                    self.qmp('qom-set', {'path': '/machine', 'property': 'mcu-pins', 'value': 'G13=1' if key == 'shutter' else 'G1=1'})
                    if key == 'shutter': self.qmp('qom-set', {'path': '/machine', 'property': 'mcu-pins', 'value': 'G1=1'})
                else:
                    prop = 'mcu-menu' if key == 'menu' else ('mcu-navigation' if key in {'up', 'down', 'left', 'right'} else 'mcu-button')
                    self.qmp('qom-set', {'path': '/machine', 'property': prop, 'value': True if key == 'menu' else key})
                    time.sleep(.07)
                    self.qmp('qom-set', {'path': '/machine', 'property': prop, 'value': False if key == 'menu' else 'none'})
                with (self.run / 'gui-inputs.jsonl').open('a', encoding='utf-8') as log:
                    log.write(json.dumps({'time': time.time(), 'key': key, 'repeat_index': index + 1}) + '\n')
                if index + 1 < repeat: time.sleep(max(.03, min(float(interval), 2)))
        return {'accepted': repeat, 'key': key}

def b64(data): return base64.b64encode(data).decode() if data else None


def tail(path, size):
    with path.open('rb') as source:
        source.seek(0, 2)
        source.seek(max(0, source.tell() - size))
        return source.read(size).decode('utf-8', errors='replace')

def png_chunk(kind, payload):
    return struct.pack('>I', len(payload)) + kind + payload + struct.pack('>I', zlib.crc32(kind + payload) & 0xffffffff)

def ppm_png(path):
    data = path.read_bytes(); pos = 2; vals = []
    while len(vals) < 3:
        while data[pos] in b' \r\n\t': pos += 1
        end = pos
        while data[end] not in b' \r\n\t': end += 1
        vals.append(int(data[pos:end])); pos = end
    width, height, _ = vals; raw = data[pos + 1:pos + 1 + width * height * 3]
    scan = b''.join(b'\0' + raw[y * width * 3:(y + 1) * width * 3] for y in range(height))
    ihdr = struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)
    return b'\x89PNG\r\n\x1a\n' + png_chunk(b'IHDR', ihdr) + png_chunk(b'IDAT', zlib.compress(scan, 1)) + png_chunk(b'IEND', b'')

S = None
HTML = ''
class Handler(BaseHTTPRequestHandler):
    def out(self, obj, code=200):
        raw = json.dumps(obj).encode(); self.send_response(code); self.send_header('Content-Type', 'application/json'); self.send_header('Content-Length', str(len(raw))); self.end_headers(); self.wfile.write(raw)
    def multipart(self):
        length = int(self.headers.get('Content-Length', '0'))
        if length <= 0 or length > 300 * 1024 * 1024:
            raise ValueError('上传请求大小不合法')
        body = self.rfile.read(length)
        message = BytesParser(policy=policy.default).parsebytes(
            b'Content-Type: ' + self.headers.get('Content-Type', '').encode() + b'\r\n\r\n' + body)
        fields = {part.get_param('name', header='content-disposition'): part
                  for part in message.iter_parts() if part.get_content_disposition() == 'form-data'}
        file_part = fields.get('file')
        if file_part is None or not file_part.get_filename():
            raise ValueError('请选择要复制的文件')
        target_part = fields.get('target')
        target = target_part.get_content().strip() if target_part else 'sd'
        return target, file_part.get_filename(), file_part.get_payload(decode=True) or b''
    def do_GET(self):
        if urlparse(self.path).path == '/':
            raw = (ROOT / 'gui.html').read_bytes(); self.send_response(200); self.send_header('Cache-Control', 'no-store'); self.send_header('Content-Type', 'text/html; charset=utf-8'); self.send_header('Content-Length', str(len(raw))); self.end_headers(); self.wfile.write(raw); return
        if urlparse(self.path).path == '/api/state':
            self.out(S.snapshot())
            return
        if urlparse(self.path).path == '/api/log':
            raw = S.logs().encode(); self.send_response(200); self.send_header('Content-Type','text/plain; charset=utf-8'); self.send_header('Content-Length',str(len(raw))); self.end_headers(); self.wfile.write(raw); return
        self.send_error(404)
    def do_POST(self):
        try:
            path = urlparse(self.path).path
            if path == '/api/sd-upload':
                target, filename, payload = self.multipart()
                self.out(S.upload(target, filename, payload))
                return
            if path == '/api/test-input-reset':
                self.out(S.reset_test_input())
                return
            if path == '/api/test-input-upload':
                _target, filename, payload = self.multipart()
                self.out(S.upload_test_input(filename, payload))
                return
            n = int(self.headers.get('Content-Length', '0')); body = json.loads(self.rfile.read(n) or b'{}')
            if path == '/api/start': result = S.start()
            elif path == '/api/stop': S.stop(); result = {'status': 'stopped'}
            elif path == '/api/input': result = S.press(body['key'], body.get('repeat', 1), body.get('interval', .26))
            elif path == '/api/control': result = S.control(body)
            else: self.send_error(404); return
            self.out(result)
        except Exception as exc:
            if urlparse(self.path).path == '/api/start':
                S.start_error = repr(exc)
                S.lifecycle = 'failed'
            self.out({'error': repr(exc)}, 500)
    def log_message(self, *_): pass

def configure(config):
    global CONFIG, GUI_MEDIA, USER_SD, USER_MICROSD, TEST_INPUT, TEST_RAW, S
    CONFIG = config
    GUI_MEDIA = config.media_dir
    USER_SD, USER_MICROSD = GUI_MEDIA / 'sd.raw', GUI_MEDIA / 'microsd.raw'
    TEST_INPUT, TEST_RAW = GUI_MEDIA / 'test-input.png', GUI_MEDIA / 'test-input.nz9raw'
    S = Session()


def serve(port=8766):
    import signal
    if not 1 <= port <= 65535:
        raise ValueError('Port must be between 1 and 65535')
    S.reset_test_input(restart=False)
    server = ThreadingHTTPServer(('127.0.0.1', port), Handler)
    def stop(*_):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, stop)
    print(f'Zf QEMU GUI: http://127.0.0.1:{port}', flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        S.stop()
        server.server_close()
