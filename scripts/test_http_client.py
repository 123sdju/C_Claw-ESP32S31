"""Exercise the real HTTP parser over fake sockets; no board or DNS access."""
import io
from concurrent.futures import ThreadPoolExecutor
import socket
import time
import unittest
from unittest.mock import Mock, patch

import long_run_test as runner


def address(ip="192.0.2.10", port=80):
    return [(socket.AF_INET, socket.SOCK_STREAM, socket.IPPROTO_TCP, "", (ip, port))]


def response(body=b"ok", connection="keep-alive"):
    return f"HTTP/1.1 200 OK\r\nContent-Length: {len(body)}\r\nConnection: {connection}\r\n\r\n".encode() + body


class FakeSocket:
    def __init__(self, responses=(), connect_error=None):
        self.responses = list(responses)
        self.connect_error = connect_error
        self.sent = []
        self.connected_to = None
        self.closed = False

    def settimeout(self, timeout):
        self.timeout = timeout

    def setsockopt(self, *args):
        pass

    def connect(self, sockaddr):
        self.connected_to = sockaddr
        if self.connect_error:
            raise self.connect_error

    def sendall(self, data):
        self.sent.append(bytes(data))

    def makefile(self, *args):
        return io.BytesIO(self.responses.pop(0))

    def close(self):
        self.closed = True


class HttpClientTests(unittest.TestCase):
    def test_parallel_clients_share_first_resolution_but_not_sockets(self):
        parent = runner.HttpClient("http://cclaw.local", 2)
        clients = [parent.fork() for _ in range(4)]
        def lookup(*args, **kwargs):
            time.sleep(.01)
            return address()
        try:
            with patch.object(runner.socket, "getaddrinfo", side_effect=lookup) as resolve, patch.object(runner.socket, "socket", side_effect=lambda *args: FakeSocket([response()])) as create:
                with ThreadPoolExecutor(max_workers=4) as pool:
                    results = list(pool.map(lambda c: c.request_text("GET", "/api/status"), clients))
            self.assertTrue(all(r[0] == 200 for r in results))
            self.assertEqual(resolve.call_count, 1)
            self.assertEqual(create.call_count, 4)
        finally:
            for client in clients:
                client.close()

    def test_one_lookup_for_actual_connection_and_shared_cache_for_workers(self):
        first = FakeSocket([response(), response(b"\xff\xd8\x00\xff\xd9")])
        second = FakeSocket([response()])
        client = runner.HttpClient("http://cclaw.local", 2)
        child = client.fork()
        try:
            with patch.object(runner.socket, "getaddrinfo", return_value=address()) as resolve, patch.object(runner.socket, "socket", side_effect=[first, second]) as create:
                self.assertEqual(client.request_text("GET", "/api/status")[1], "ok")
                self.assertEqual(client.request_bytes("GET", "/api/file", max_bytes=100)[1], b"\xff\xd8\x00\xff\xd9")
                child.request_text("POST", "/api/messages", body="/lua print(1)")
            self.assertEqual(resolve.call_count, 1)
            self.assertEqual(create.call_count, 2)
            self.assertTrue(client.last_timing["connection_reused"])
            self.assertTrue(child.last_timing["dns_cache_hit"])
            self.assertIs(client.resolver, child.resolver)
            self.assertIn(b"Host: cclaw.local\r\n", first.sent[0])
        finally:
            client.close()
            child.close()

    def test_nonpersistent_requests_still_share_dns_and_use_separate_sockets(self):
        sockets = [FakeSocket([response()]), FakeSocket([response()])]
        client = runner.HttpClient("http://cclaw.local", 2, persistent=False)
        with patch.object(runner.socket, "getaddrinfo", return_value=address()) as resolve, patch.object(runner.socket, "socket", side_effect=sockets):
            client.request_text("GET", "/api/status")
            client.request_text("GET", "/api/status")
        self.assertEqual(resolve.call_count, 1)
        self.assertTrue(all(s.closed for s in sockets))
        self.assertIn(b"Connection: close\r\n", sockets[0].sent[0])

    def test_dns_retry_happens_before_any_post_is_sent(self):
        sock = FakeSocket([response()])
        client = runner.HttpClient("http://cclaw.local", 2)
        try:
            with patch.object(runner.socket, "getaddrinfo", side_effect=[socket.gaierror(11001, "getaddrinfo failed"), address()]) as resolve, patch.object(runner.socket, "socket", return_value=sock), patch.object(runner.time, "sleep"):
                client.request_text("POST", "/api/messages", body="once")
            self.assertEqual(resolve.call_count, 2)
            self.assertEqual(client.last_timing["dns_lookup_failures"], 1)
            self.assertEqual(sum(b"POST " in data for data in sock.sent), 1)
        finally:
            client.close()

    def test_persistent_dns_failure_is_bounded_and_sends_nothing(self):
        client = runner.HttpClient("http://cclaw.local", 2)
        with patch.object(runner.socket, "getaddrinfo", side_effect=socket.gaierror(11001, "getaddrinfo failed")) as resolve, patch.object(runner.socket, "socket") as create, patch.object(runner.time, "sleep"):
            with self.assertRaises(socket.gaierror):
                client.request_text("POST", "/api/messages", body="once")
        self.assertEqual(resolve.call_count, 2)
        create.assert_not_called()
        self.assertEqual(client.last_timing["dns_lookup_failures"], 2)
        self.assertIsNone(client._conn)

    def test_failed_cached_address_is_refreshed_before_post_dispatch(self):
        client = runner.HttpClient("http://cclaw.local", 2)
        stale = FakeSocket(connect_error=ConnectionRefusedError("stale DHCP address"))
        fresh = FakeSocket([response()])
        try:
            with patch.object(runner.socket, "getaddrinfo", side_effect=[address(), address("192.0.2.11")]) as resolve, patch.object(runner.socket, "socket", side_effect=[stale, fresh]):
                client.resolver.resolve("cclaw.local", 80, {})
                client.request_text("POST", "/api/messages", body="once")
            self.assertEqual(resolve.call_count, 2)
            self.assertEqual(stale.sent, [])
            self.assertTrue(stale.closed)
            self.assertEqual(fresh.connected_to, ("192.0.2.11", 80))
            self.assertEqual(sum(b"POST " in data for data in fresh.sent), 1)
        finally:
            client.close()

    def test_response_failure_never_replays_post(self):
        sock = FakeSocket([b"HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort"])
        client = runner.HttpClient("http://cclaw.local", 2)
        with patch.object(runner.socket, "getaddrinfo", return_value=address()) as resolve, patch.object(runner.socket, "socket", return_value=sock) as create:
            with self.assertRaises(runner.http.client.IncompleteRead):
                client.request_text("POST", "/api/messages", body="once")
        self.assertEqual(create.call_count, 1)
        self.assertEqual(resolve.call_count, 1)
        self.assertEqual(sum(b"POST " in data for data in sock.sent), 1)
        self.assertTrue(sock.closed)

    def test_https_preserves_original_hostname_for_sni_and_host(self):
        sock = FakeSocket([response()])
        tls = Mock()
        tls.wrap_socket.side_effect = lambda sock, **kwargs: sock
        original_init = runner.http.client.HTTPSConnection.__init__
        def init_with_context(connection, host, port, timeout):
            original_init(connection, host, port, timeout=timeout, context=tls)
        client = runner.HttpClient("https://cclaw.local", 2)
        try:
            with patch.object(runner.http.client.HTTPSConnection, "__init__", new=init_with_context), patch.object(runner.socket, "getaddrinfo", return_value=address(port=443)), patch.object(runner.socket, "socket", return_value=sock):
                client.request_text("GET", "/api/status")
            tls.wrap_socket.assert_called_once_with(sock, server_hostname="cclaw.local")
            self.assertIn(b"Host: cclaw.local\r\n", sock.sent[0])
        finally:
            client.close()

    def test_oversized_binary_response_closes_connection(self):
        sock = FakeSocket([response(b"0123456789")])
        client = runner.HttpClient("http://cclaw.local", 2)
        with patch.object(runner.socket, "getaddrinfo", return_value=address()), patch.object(runner.socket, "socket", return_value=sock):
            with self.assertRaisesRegex(ValueError, "exceeds 4 bytes"):
                client.request_bytes("GET", "/api/file", max_bytes=4)
        self.assertTrue(sock.closed)
        self.assertIsNone(client._conn)

    def test_expired_address_is_resolved_again(self):
        resolver = runner.AddressResolver(ttl=0)
        with patch.object(runner.socket, "getaddrinfo", side_effect=[address(), address("192.0.2.11")]) as resolve:
            resolver.resolve("cclaw.local", 80, {})
            addresses, cached = resolver.resolve("cclaw.local", 80, {})
        self.assertEqual(resolve.call_count, 2)
        self.assertFalse(cached)
        self.assertEqual(addresses[0][-1][0], "192.0.2.11")

    def test_session_cache_survives_time_but_allows_failure_invalidation(self):
        resolver = runner.AddressResolver(ttl=float("inf"))
        with patch.object(runner.socket, "getaddrinfo", side_effect=[address(), address("192.0.2.11")]) as resolve:
            with patch.object(runner.time, "monotonic", return_value=1):
                first, _ = resolver.resolve("cclaw.local", 80, {})
            with patch.object(runner.time, "monotonic", return_value=3601):
                same, cached = resolver.resolve("cclaw.local", 80, {})
                self.assertIs(same, first)
                self.assertTrue(cached)
                self.assertEqual(resolve.call_count, 1)
                resolver.invalidate("cclaw.local", 80, same)
                fresh, cached = resolver.resolve("cclaw.local", 80, {})
        self.assertEqual(resolve.call_count, 2)
        self.assertFalse(cached)
        self.assertEqual(fresh[0][-1][0], "192.0.2.11")

    def test_bounded_binary_read_rejects_incomplete_content_length(self):
        sock = FakeSocket([b"HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n\xff\xd8\xff\xd9"])
        client = runner.HttpClient("http://cclaw.local", 2)
        with patch.object(runner.socket, "getaddrinfo", return_value=address()), patch.object(runner.socket, "socket", return_value=sock):
            with self.assertRaises(runner.http.client.IncompleteRead):
                client.request_bytes("GET", "/api/file", max_bytes=1024)
        self.assertTrue(sock.closed)


if __name__ == "__main__":
    unittest.main()
