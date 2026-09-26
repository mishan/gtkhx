"""Just enough Hotline to populate a server for the screenshots.

A bot logs in, sets its nick color and GIF icon, and talks, including an
inline image. A fake tracker serves a fixed v1 listing. Everything reads
its replies on a thread of its own, so the server never stalls writing to
a bot.
"""

import socket
import struct
import threading

# Transactions.
CHAT_SEND = 105
USER_CHANGE = 301
LOGIN = 107
AGREED = 121
SET_USER_INFO = 304
UPLOAD_MEDIA = 750
ICON_SET = 1862

# Fields.
DATA_CHAT = 0x0065
DATA_NAME = 0x0066
DATA_ICON = 0x0068
DATA_LOGIN = 0x0069
DATA_STYLE = 0x006D
DATA_OPTIONS = 0x0071
DATA_VERSION = 0x00A0
DATA_CAPABILITIES = 0x01F0
DATA_MEDIA_TYPE = 0x0201
DATA_MEDIA_ID = 0x0202
DATA_MEDIA_PAYLOAD = 0x0203
DATA_MEDIA_DECLARED_TYPE = 0x0204
DATA_MEDIA_PART_FINAL = 0x020B
DATA_ICON_GIF = 0x0300
DATA_COLOR = 0x0500

CAP_INLINE_MEDIA = 0x0008


def u16(n):
    return struct.pack(">H", n)


def u32(n):
    return struct.pack(">I", n)


def pack_fields(fields):
    body = u16(len(fields))
    for fid, data in fields:
        body += u16(fid) + u16(len(data)) + data
    return body


def parse_fields(body):
    if len(body) < 2:
        return {}
    count, = struct.unpack_from(">H", body)
    at, out = 2, {}
    for _ in range(count):
        fid, size = struct.unpack_from(">HH", body, at)
        out[fid] = body[at + 4: at + 4 + size]
        at += 4 + size
    return out


class Bot:
    """One scripted user on a server."""

    def __init__(self, host, port, nick, icon, caps=CAP_INLINE_MEDIA):
        self.nick = nick
        self.sock = socket.create_connection((host, port), timeout=10)
        self.sock.sendall(b"TRTPHOTL" + u16(1) + u16(2))
        if self._read(8)[:4] != b"TRTP":
            raise OSError(f"{host}:{port} didn't answer as a Hotline server")
        self.trans = 1
        self.replies = {}
        self.arrived = set()
        self.cond = threading.Condition()
        self.sock.settimeout(None)
        threading.Thread(target=self._reader, daemon=True).start()
        self.request(LOGIN, [
            (DATA_NAME, nick.encode()), (DATA_ICON, u16(icon)),
            (DATA_VERSION, u16(185)), (DATA_CAPABILITIES, u16(caps)),
        ], wait=True)
        self.request(AGREED, [
            (DATA_ICON, u16(icon)), (DATA_NAME, nick.encode()), (DATA_OPTIONS, u16(0)),
        ])
        self.icon = icon

    def _read(self, n):
        buf = b""
        while len(buf) < n:
            got = self.sock.recv(n - len(buf))
            if not got:
                raise OSError("the server closed the connection")
            buf += got
        return buf

    def _reader(self):
        try:
            while True:
                head = self._read(20)
                _, is_reply, kind, trans, err, _, size = struct.unpack(">BBHIIII", head)
                body = self._read(size)
                fields = parse_fields(body)
                with self.cond:
                    if is_reply:
                        self.replies[trans] = (err, fields)
                    elif kind == USER_CHANGE and DATA_NAME in fields:
                        self.arrived.add(fields[DATA_NAME].decode(errors="replace"))
                    self.cond.notify_all()
        except OSError:
            pass

    def request(self, kind, fields, wait=False, timeout=15):
        """Send a transaction; with `wait`, return its reply's fields."""
        trans = self.trans
        self.trans += 1
        body = pack_fields(fields)
        head = struct.pack(">BBHIIII", 0, 0, kind, trans, 0, len(body), len(body))
        self.sock.sendall(head + body)
        if not wait:
            return None
        with self.cond:
            if not self.cond.wait_for(lambda: trans in self.replies, timeout):
                raise OSError(f"{self.nick}: no reply to transaction {kind}")
            err, reply = self.replies.pop(trans)
        if err:
            raise OSError(f"{self.nick}: transaction {kind} failed: "
                          f"{reply.get(DATA_CHAT, b'').decode(errors='replace')}")
        return reply

    def saw_user(self, nick):
        """Whether the server has announced `nick` arriving or changing."""
        with self.cond:
            return nick in self.arrived

    def set_color(self, rgb):
        """Nick color, as 0xRRGGBB. Janus's colored-nicknames extension."""
        self.request(SET_USER_INFO, [
            (DATA_NAME, self.nick.encode()), (DATA_ICON, u16(self.icon)),
            (DATA_COLOR, u32(rgb)),
        ])   # 304 has no reply

    def set_gif_icon(self, gif):
        self.request(ICON_SET, [(DATA_ICON_GIF, gif)], wait=True)

    def say(self, text, image=None, mime="image/png"):
        fields = [(DATA_CHAT, text.encode())]   # a style of 1 would be /me
        if image is not None:
            reply = self.request(UPLOAD_MEDIA, [
                (DATA_MEDIA_PAYLOAD, image), (DATA_MEDIA_DECLARED_TYPE, mime.encode()),
                (DATA_MEDIA_PART_FINAL, b"\x01"),
            ], wait=True)
            fields += [(DATA_MEDIA_ID, reply[DATA_MEDIA_ID]),
                       (DATA_MEDIA_TYPE, reply.get(DATA_MEDIA_TYPE, mime.encode()))]
        self.request(CHAT_SEND, fields)   # 105 has no reply

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def tracker_listing(servers):
    """A v1 tracker reply: the header, then one record a server."""
    records = b""
    for s in servers:
        name, desc = s["name"].encode(), s["desc"].encode()
        records += (socket.inet_aton(s["addr"]) + u16(s["port"]) + u16(s["users"])
                    + b"\0\0" + bytes([len(name)]) + name + bytes([len(desc)]) + desc)
    n = len(servers)
    return b"HTRK" + u16(1) + u16(1) + u16(len(records) + 4) + u16(n) + u16(n) + records


def serve_tracker(port, servers):
    """Answer every tracker request with `servers`, on a thread. A client
    tries TLS first: anything that doesn't open with HTRK is dropped, and
    it falls back to plain TCP."""
    reply = tracker_listing(servers)
    listener = socket.create_server(("127.0.0.1", port))

    def answer(conn):
        with conn:
            conn.settimeout(10)
            try:
                if conn.recv(4, socket.MSG_WAITALL) != b"HTRK":
                    return
                conn.recv(2, socket.MSG_WAITALL)
                conn.sendall(reply)
                while conn.recv(256):
                    pass
            except OSError:
                pass

    def loop():
        while True:
            conn, _ = listener.accept()
            threading.Thread(target=answer, args=(conn,), daemon=True).start()

    threading.Thread(target=loop, daemon=True).start()
    return listener
