#!/usr/bin/env python3
"""Pruebas de integración del límite, timeout y shutdown de MNG/1."""

import contextlib
import os
import signal
import socket
import struct
import subprocess
import threading
import time


ADMIN_FRAME = b"\x01\x05admin\x04pass"


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def recv_exact(sock, count):
    data = bytearray()
    while len(data) < count:
        chunk = sock.recv(count - len(data))
        if not chunk:
            raise EOFError(f"EOF tras {len(data)}/{count} bytes")
        data.extend(chunk)
    return bytes(data)


def assert_closed(sock, message):
    try:
        data = sock.recv(1)
    except ConnectionResetError:
        data = b""
    assert data == b"", message


class Server:
    def __init__(self):
        self.socks_port = free_port()
        self.mng_port = free_port()
        self.process = None

    def __enter__(self):
        self.process = subprocess.Popen(
            [
                "./bin/server",
                "-l", "127.0.0.1",
                "-L", "127.0.0.1",
                "-p", str(self.socks_port),
                "-P", str(self.mng_port),
                "-A", "admin:pass",
            ],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )
        deadline = time.time() + 3
        while time.time() < deadline:
            try:
                with socket.create_connection(
                    ("127.0.0.1", self.mng_port), timeout=0.2
                ):
                    return self
            except OSError:
                if self.process.poll() is not None:
                    raise RuntimeError(self.process.stderr.read())
                time.sleep(0.03)
        raise TimeoutError("el servidor no abrió el puerto MNG")

    def stop(self, timeout=5):
        if self.process.poll() is None:
            os.kill(self.process.pid, signal.SIGINT)
            try:
                self.process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
                raise
        if self.process.returncode != 0:
            raise RuntimeError(self.process.stderr.read())

    def __exit__(self, exc_type, exc_value, traceback):
        if self.process.poll() is None:
            if exc_type is None:
                self.stop()
            else:
                self.process.kill()
                self.process.wait()


def mng_connect(port):
    sock = socket.create_connection(("127.0.0.1", port), timeout=2)
    sock.sendall(ADMIN_FRAME)
    if recv_exact(sock, 2) != b"\x01\x00":
        sock.close()
        raise AssertionError("falló la autenticación MNG")
    return sock


def test_limit_and_shutdown():
    with Server() as server, contextlib.ExitStack() as stack:
        connections = [
            stack.enter_context(contextlib.closing(mng_connect(server.mng_port)))
            for _ in range(16)
        ]
        extra = stack.enter_context(
            contextlib.closing(
                socket.create_connection(("127.0.0.1", server.mng_port), timeout=2)
            )
        )
        extra.sendall(ADMIN_FRAME)
        try:
            response = extra.recv(2)
        except ConnectionResetError:
            response = b""
        assert response == b"", "la sesión MNG número 17 no fue rechazada"

        os.kill(server.process.pid, signal.SIGINT)
        assert server.process.wait(timeout=2) == 0
        for connection in connections:
            connection.settimeout(1)
            assert_closed(connection, "el shutdown no cerró una sesión MNG")


def test_timeout_releases_slot():
    with Server() as server, contextlib.ExitStack() as stack:
        connection = stack.enter_context(
            contextlib.closing(mng_connect(server.mng_port))
        )
        connection.sendall(b"\x05" + struct.pack("!I", 1))
        assert recv_exact(connection, 1) == b"\x00"
        connection.settimeout(12)
        assert_closed(connection, "el timeout no cerró la sesión MNG")
        stack.enter_context(contextlib.closing(mng_connect(server.mng_port)))


class Origin:
    def __init__(self):
        self.listener = socket.socket()
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(600)
        self.listener.settimeout(0.2)
        self.port = self.listener.getsockname()[1]
        self.connections = []
        self.running = True
        self.thread = threading.Thread(target=self._accept, daemon=True)

    def _accept(self):
        while self.running:
            try:
                self.connections.append(self.listener.accept()[0])
            except socket.timeout:
                pass
            except OSError:
                return

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *_):
        self.running = False
        self.listener.close()
        for connection in self.connections:
            connection.close()
        self.thread.join(timeout=1)


def test_500_socks_with_16_mng():
    with Server() as server, Origin() as origin, contextlib.ExitStack() as stack:
        for _ in range(16):
            stack.enter_context(contextlib.closing(mng_connect(server.mng_port)))

        request = (
            b"\x05\x01\x00\x01"
            + socket.inet_aton("127.0.0.1")
            + struct.pack("!H", origin.port)
        )
        for index in range(500):
            client = stack.enter_context(
                contextlib.closing(
                    socket.create_connection(
                        ("127.0.0.1", server.socks_port), timeout=3
                    )
                )
            )
            client.sendall(b"\x05\x01\x00")
            assert recv_exact(client, 2) == b"\x05\x00", index
            client.sendall(request)
            assert recv_exact(client, 10)[:2] == b"\x05\x00", index

        deadline = time.time() + 3
        while len(origin.connections) < 500 and time.time() < deadline:
            time.sleep(0.01)
        assert len(origin.connections) == 500


if __name__ == "__main__":
    test_limit_and_shutdown()
    print("límite de 16 y graceful shutdown: OK")
    test_timeout_releases_slot()
    print("timeout MNG y reutilización del slot: OK")
    test_500_socks_with_16_mng()
    print("16 MNG + 500 SOCKS CONNECT: OK")
