"""MCP SDK 2.x runs synchronous tool handlers on worker threads, so concurrent
tool calls can share one UnrealConnection. Commands must not clobber each
other's sockets."""

import json
import socket
import threading
import time

import unreal_mcp_server
from unreal_mcp_server import UnrealConnection


def _start_slow_echo_server(delay_seconds: float):
    """One-command-per-connection server, like the Unreal bridge."""
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(8)
    stop = threading.Event()

    def handle(conn):
        with conn:
            request = json.loads(conn.recv(65536).decode("utf-8"))
            time.sleep(delay_seconds)
            reply = {"status": "success", "result": {"echo": request["params"]["n"]}}
            conn.sendall(json.dumps(reply).encode("utf-8"))

    def serve():
        listener.settimeout(0.2)
        while not stop.is_set():
            try:
                conn, _ = listener.accept()
            except socket.timeout:
                continue
            threading.Thread(target=handle, args=(conn,), daemon=True).start()
        listener.close()

    threading.Thread(target=serve, daemon=True).start()
    return listener.getsockname()[1], stop


def test_concurrent_commands_on_shared_connection_do_not_clobber_sockets(monkeypatch):
    port, stop = _start_slow_echo_server(delay_seconds=0.2)
    monkeypatch.setattr(unreal_mcp_server, "UNREAL_PORT", port)
    connection = UnrealConnection(connect_timeout_seconds=2.0, receive_timeout_seconds=5.0)

    results = {}

    def call(n):
        results[n] = connection.send_command("echo", {"n": n})

    try:
        threads = [threading.Thread(target=call, args=(n,)) for n in range(4)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(timeout=10)
    finally:
        stop.set()

    for n in range(4):
        assert results.get(n) == {"status": "success", "result": {"echo": n}}, results
