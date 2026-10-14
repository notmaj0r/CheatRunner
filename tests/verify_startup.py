#!/usr/bin/env python3
"""Run with Python 3 and a host C compiler; no PS5 or third-party packages needed.

Compile the startup socket helpers directly from main.c. An optional source path
allows the same regression test to be run against an unmodified checkout.
"""
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time

source = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1] / 'src/main.c'
src = source.read_text()
# Include connect_loopback when present, or the original port_answers otherwise.
start = src.index('static int\n' + ('connect_loopback' if 'connect_loopback' in src else 'port_answers'))
end = src.index('\nint\nmain(void)', start)
prefix = '''#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
static void cr_log(const char *level, const char *tag, const char *fmt, ...) {
 (void)level; (void)tag; (void)fmt;
}
'''
suffix = '\nint main(int argc, char **argv) { if (argc != 2) return 1; evict_old_instance(atoi(argv[1])); return 0; }\n'


def exercise(binary, mode):
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        port = listener.getsockname()[1]
        # A bound, non-listening port deterministically refuses connections.
        if mode == 'refused':
            subprocess.run([str(binary), str(port)], check=True, timeout=2)
            print('PASS: no listener returns promptly')
            return
        listener.listen()
        listener.settimeout(0.1)
        stop = threading.Event()
        clients, requests, errors = [], [], []

        def serve():
            try:
                while not stop.is_set():
                    try:
                        client, _ = listener.accept()
                    except TimeoutError:
                        continue
                    clients.append(client)
                    client.settimeout(1)
                    data = client.recv(4096)
                    if not data:  # port_answers connectivity probe
                        client.close()
                        continue
                    requests.append(data)
                    if mode == 'responsive':
                        client.sendall(b'HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n')
                        client.close()
                    # In silent mode, retain the connection without responding.
            except Exception as error:
                errors.append(error)

        thread = threading.Thread(target=serve)
        thread.start()
        try:
            began = time.monotonic()
            subprocess.run([str(binary), str(port)], check=True, timeout=8)
            elapsed = time.monotonic() - began
        finally:
            stop.set()
            thread.join(timeout=2)
            for client in clients:
                client.close()
        assert not thread.is_alive(), 'server thread did not stop'
        assert not errors, errors
        assert len(requests) == 2, requests
        assert all(r.startswith(b'POST /api/dev/shutdown HTTP/1.0\r\n') for r in requests)
        print(f'PASS: {mode} server, two shutdown attempts, returned in {elapsed:.2f}s')


with tempfile.TemporaryDirectory(prefix='cr-startup-') as tmp:
    path = Path(tmp)
    (path / 'test.c').write_text(prefix + src[start:end] + suffix)
    subprocess.run(['cc', '-D_DEFAULT_SOURCE', '-Wall', '-Wextra', '-Werror', str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    for mode in ('refused', 'responsive', 'silent'):
        exercise(path / 'test', mode)
