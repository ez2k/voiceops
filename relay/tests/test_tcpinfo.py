"""TCP_INFO 읽기: 실제 TCP 연결(127.0.0.1)로 커널 값과 맞춰 본다."""

import socket
import sys
import time

import pytest

from geminirelay import tcpinfo

pytestmark = pytest.mark.skipif(not sys.platform.startswith("linux"), reason="TCP_INFO 는 리눅스에만 있다")


def test_reads_what_the_peer_acked():
    with socket.create_server(("127.0.0.1", 0)) as srv:
        a = socket.create_connection(srv.getsockname())
        b, _ = srv.accept()
        with a, b:
            before = tcpinfo.read(a)
            a.sendall(b"x" * 100_000)
            got = 0
            while got < 100_000:
                got += len(b.recv(65536))
            for _ in range(100):  # 받은 쪽의 ACK 가 돌아올 때까지
                after = tcpinfo.read(a)
                if after.bytes_acked - before.bytes_acked >= 100_000:
                    break
                time.sleep(0.01)
            assert after.bytes_acked - before.bytes_acked == 100_000
            assert (after.total_retrans, after.backoff, after.unacked, after.notsent) == (0, 0, 0, 0)
            assert 200 <= after.rto_ms < 1000 and after.snd_mss > 0
            line = tcpinfo.describe(before, after, 1.0, buffered=1024)
            assert line.startswith("기기 쪽 TCP: 받음 97.7KB/s · 재전송 0회 · ")
            assert "밀림 1.0KB" in line


def test_closed_socket_gives_none():
    s = socket.socket()
    s.close()
    assert tcpinfo.read(s) is None
