"""Short-lived local Unix QMP connections shared by both frontends."""
import json
import socket


def query(run, command, arguments=None):
    with socket.socket(socket.AF_UNIX) as connection:
        connection.settimeout(10)
        connection.connect(str(run / 'qmp.sock'))
        stream = connection.makefile('rwb', buffering=0)
        if not stream.readline():
            raise ConnectionError('QMP greeting missing')
        for request in ({'execute': 'qmp_capabilities'},
                        {'execute': command, 'arguments': arguments or {}}):
            stream.write((json.dumps(request) + '\n').encode())
            while True:
                line = stream.readline()
                if not line:
                    raise ConnectionError('QMP connection closed')
                reply = json.loads(line)
                if 'event' not in reply:
                    break
            if 'error' in reply:
                raise RuntimeError(reply['error'])
        return reply.get('return')
