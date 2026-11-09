#!/usr/bin/env python3
"""A minimal Kaillera server (protocol 0.83) for testing netplay locally:
a Wii in Dolphin and GEMU on this computer, without a public server.

    tools/kaillera_test_server.py [--port 27888] [--delay N] [--log FILE]

Speaks the part of the protocol GEMU's client (core/src/netplay_kaillera.c)
uses: HELLO / PING on the main port, then on the session port login, the
lobby (users, games, chat), create / join / quit / close game, start,
ready, game data and drops.  Game data is combined the way EmuLinker-K
does it - every player's frame n together, in player order - and sent to
each player once everyone's input for those frames is in; no cache
(0x13) messages.  --delay is the delay value handed out at start (the
client plays (delay + 1) * connection - 1 frames of no input first).

It's a test tool: one process, no limits, no security.  Not for running a
public server.
"""
import argparse, socket, struct, sys, time

M = dict(USER_QUIT=1, USER_JOINED=2, LOGIN=3, SERVER_STATUS=4, SERVER_ACK=5, CLIENT_ACK=6,
         CHAT=7, GAME_CHAT=8, KEEPALIVE=9, CREATE_GAME=10, QUIT_GAME=11, JOIN_GAME=12,
         PLAYER_INFO=13, GAME_STATUS=14, KICK=15, CLOSE_GAME=16, START_GAME=17, GAME_DATA=18,
         GAME_CACHE=19, DROP_GAME=20, READY=21, REJECTED=22, INFO=23)
NAMES = {v: k for k, v in M.items()}
HIST = 5


def s(text):
    return text.encode("latin-1", "replace") + b"\0"


class Rd:
    def __init__(self, b): self.b, self.i = b, 0
    def u8(self):
        v = self.b[self.i]; self.i += 1; return v
    def u16(self):
        v = struct.unpack_from("<H", self.b, self.i)[0]; self.i += 2; return v
    def u32(self):
        v = struct.unpack_from("<I", self.b, self.i)[0]; self.i += 4; return v
    def str(self):
        j = self.b.index(b"\0", self.i); v = self.b[self.i:j].decode("latin-1"); self.i = j + 1; return v
    def rest(self): return self.b[self.i:]


class Client:
    def __init__(self, addr, uid):
        self.addr, self.id = addr, uid
        self.name, self.emulator, self.conn = "", "", 1
        self.in_last = -1
        self.out_seq = 0
        self.hist = []            # (seq, msg bytes), newest first
        self.logged_in = False
        self.game = None
        self.player = 0
        self.ready = False
        self.inputs = bytearray()  # this player's frames, 2 bytes each
        self.last_rx = time.time()


class Game:
    def __init__(self, gid, name, owner):
        self.id, self.name, self.owner = gid, name, owner
        self.emulator = owner.emulator
        self.players = [owner]
        self.status = 0           # 0 waiting, 1 playing, 2 synchronizing
        self.sent = 0             # combined frames sent so far


class Server:
    def __init__(self, port, delay, log):
        self.delay, self.log = delay, log
        self.main = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.main.bind(("0.0.0.0", port))
        self.sess = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sess.bind(("0.0.0.0", port + 1))
        self.sess_port = port + 1
        self.clients = {}
        self.games = {}
        self.next_uid, self.next_gid = 1, 1
        self.say(f"Kaillera test server on UDP {port} (session port {port + 1}), delay {delay}")

    def say(self, msg):
        line = time.strftime("%H:%M:%S ") + msg
        print(line, flush=True)
        if self.log:
            with open(self.log, "a") as f: f.write(line + "\n")

    # ── sending ──
    def send(self, c, mtype, body=b""):
        msg = bytes([mtype]) + body
        c.hist.insert(0, (c.out_seq, msg))
        del c.hist[HIST:]
        c.out_seq = (c.out_seq + 1) & 0xFFFF
        self.flush(c)

    def flush(self, c):
        pkt = bytes([len(c.hist)])
        for seq, msg in c.hist:
            pkt += struct.pack("<HH", seq, len(msg)) + msg
        self.sess.sendto(pkt, c.addr)

    def to_all(self, mtype, body, who=None):
        for c in list(self.clients.values()):
            if c.logged_in and (who is None or c in who): self.send(c, mtype, body)

    # ── lobby ──
    def game_status(self, g):
        body = b"\0" + struct.pack("<IBBB", g.id, g.status, len(g.players), 2)
        self.to_all(M["GAME_STATUS"], body)

    def status_for(self, c):
        users = [u for u in self.clients.values() if u.logged_in and u is not c]
        body = s("") + struct.pack("<II", len(users), len(self.games))
        for u in users:
            body += s(u.name) + struct.pack("<IBHB", 20, 0 if u.game else 1, u.id, u.conn)
        for g in self.games.values():
            body += s(g.name) + struct.pack("<I", g.id) + s(g.emulator) + s(g.owner.name)
            body += s(f"{len(g.players)}/2") + bytes([g.status])
        self.send(c, M["SERVER_STATUS"], body)

    def leave_game(self, c, dropped):
        g = c.game
        if not g: return
        if dropped and g.status != 0:
            self.to_all(M["DROP_GAME"], s(c.name) + bytes([c.player]), g.players)
        self.to_all(M["QUIT_GAME"], s(c.name) + struct.pack("<H", c.id), g.players)
        g.players.remove(c)
        c.game, c.player, c.ready, c.inputs = None, 0, False, bytearray()
        if c is g.owner or not g.players:
            for p in g.players:
                p.game, p.player, p.ready, p.inputs = None, 0, False, bytearray()
            del self.games[g.id]
            self.to_all(M["CLOSE_GAME"], b"\0" + struct.pack("<I", g.id))
            self.say(f"game {g.id} closed")
        else:
            self.game_status(g)

    def quit(self, c, why):
        self.leave_game(c, True)
        self.clients.pop(c.addr, None)
        if c.logged_in:
            self.to_all(M["USER_QUIT"], s(c.name) + struct.pack("<H", c.id) + s(why))
        self.say(f"{c.name or c.addr} quit ({why})")

    # ── game data: frame n of every player together, in player order ──
    def pump(self, g):
        if g.status != 1: return
        conn = min(p.conn for p in g.players)
        have = min(len(p.inputs) // 2 for p in g.players)
        while have - g.sent >= conn:
            data = b""
            for f in range(g.sent, g.sent + conn):
                for p in g.players: data += bytes(p.inputs[2 * f:2 * f + 2])
            g.sent += conn
            for p in g.players:
                self.send(p, M["GAME_DATA"], b"\0" + struct.pack("<H", len(data)) + data)

    # ── messages from a client ──
    def handle(self, c, mtype, r):
        name = NAMES.get(mtype, hex(mtype))
        if mtype == M["LOGIN"]:
            c.name, c.emulator, c.conn = r.str(), r.str(), r.u8()
            self.say(f"login {c.name!r} ({c.emulator}, connection {c.conn}) from {c.addr}")
            self.send(c, M["SERVER_ACK"], b"\0" + struct.pack("<IIII", 0, 1, 2, 3))
        elif mtype == M["CLIENT_ACK"]:
            if not c.logged_in:
                c.logged_in = True
                self.status_for(c)
                self.to_all(M["USER_JOINED"], s(c.name) + struct.pack("<HIB", c.id, 20, c.conn))
                self.send(c, M["INFO"], s("Server") + s("Welcome to the GEMU test server"))
        elif mtype == M["CHAT"]:
            r.str(); text = r.str()
            self.say(f"<{c.name}> {text}")
            self.to_all(M["CHAT"], s(c.name) + s(text))
        elif mtype == M["GAME_CHAT"]:
            r.str(); text = r.str()
            if c.game:
                if not text.startswith("~gemu-sync "): self.say(f"[game {c.game.id}] <{c.name}> {text}")
                else: self.say(f"sync from {c.name}: {text[11:]}")
                self.to_all(M["GAME_CHAT"], s(c.name) + s(text), c.game.players)
        elif mtype == M["CREATE_GAME"]:
            r.str(); gname = r.str()
            if c.game: return
            g = Game(self.next_gid, gname, c); self.next_gid += 1
            self.games[g.id] = g
            c.game, c.player = g, 1
            self.say(f"{c.name} created game {g.id}: {gname}")
            self.to_all(M["CREATE_GAME"], s(c.name) + s(gname) + s(c.emulator) + struct.pack("<I", g.id))
            self.game_status(g)
        elif mtype == M["JOIN_GAME"]:
            r.u8(); gid = r.u32()
            g = self.games.get(gid)
            if not g or c.game or g.status != 0: return
            if g.emulator != c.emulator:
                self.send(c, M["INFO"], s("Server") + s(f"That game is for {g.emulator}")); return
            body = b"\0" + struct.pack("<I", len(g.players))
            for p in g.players: body += s(p.name) + struct.pack("<IHB", 20, p.id, p.conn)
            self.send(c, M["PLAYER_INFO"], body)
            g.players.append(c)
            c.game, c.player = g, len(g.players)
            self.say(f"{c.name} joined game {g.id}")
            self.to_all(M["JOIN_GAME"], b"\0" + struct.pack("<I", g.id) + s(c.name) +
                        struct.pack("<IHB", 20, c.id, c.conn), g.players)
            self.game_status(g)
        elif mtype == M["QUIT_GAME"]:
            self.say(f"{c.name} left game {c.game.id if c.game else '-'}")
            self.leave_game(c, False)
        elif mtype == M["START_GAME"]:
            g = c.game
            if not g or g.owner is not c or g.status != 0: return
            g.status, g.sent = 2, 0
            for i, p in enumerate(g.players, 1):
                p.player, p.ready, p.inputs = i, False, bytearray()
                self.send(p, M["START_GAME"], b"\0" + struct.pack("<HBB", self.delay, i, len(g.players)))
            self.say(f"game {g.id} starting: " + ", ".join(p.name for p in g.players))
            self.game_status(g)
        elif mtype == M["READY"]:
            g = c.game
            if not g or g.status != 2: return
            c.ready = True
            self.say(f"{c.name} is ready")
            if all(p.ready for p in g.players):
                g.status = 1
                self.to_all(M["READY"], b"\0", g.players)
                self.say(f"game {g.id} on")
                self.game_status(g)
        elif mtype == M["GAME_DATA"]:
            r.u8(); n = r.u16(); data = r.rest()[:n]
            if c.game and c.game.status == 1:
                c.inputs += data
                self.pump(c.game)
        elif mtype == M["DROP_GAME"]:
            g = c.game
            if g and g.status != 0:
                self.say(f"{c.name} dropped from game {g.id}")
                self.to_all(M["DROP_GAME"], s(c.name) + bytes([c.player]), g.players)
        elif mtype == M["USER_QUIT"]:
            r.u8(); r.u16(); why = r.str() if r.i < len(r.b) else ""
            self.quit(c, why or "quit")
        elif mtype == M["KEEPALIVE"]:
            pass
        else:
            self.say(f"{c.name}: unhandled {name}")

    def session_packet(self, data, addr):
        c = self.clients.get(addr)
        if not c:
            c = self.clients[addr] = Client(addr, self.next_uid); self.next_uid += 1
        c.last_rx = time.time()
        if not data: return
        count, o, msgs = data[0], 1, []
        for _ in range(count):
            if o + 4 > len(data): break
            seq, ln = struct.unpack_from("<HH", data, o); o += 4
            msgs.append((seq, data[o:o + ln])); o += ln
        new = False
        for seq, msg in reversed(msgs):              # oldest first
            if c.in_last >= 0:
                ahead = (seq - c.in_last) & 0xFFFF
                if ahead == 0 or ahead > 0x8000: continue
            c.in_last = seq
            new = True
            try:
                self.handle(c, msg[0], Rd(msg[1:]))
            except (IndexError, ValueError, struct.error) as e:
                self.say(f"{c.name}: bad {NAMES.get(msg[0], msg[0])}: {e}")
            if c.addr not in self.clients: return
        if not new and c.hist:                       # a resend: they missed ours
            self.flush(c)

    def run(self):
        import select
        while True:
            rd, _, _ = select.select([self.main, self.sess], [], [], 1.0)
            for sock in rd:
                data, addr = sock.recvfrom(65536)
                if sock is self.main:
                    if data.startswith(b"HELLO0.83"):
                        self.main.sendto(f"HELLOD00D{self.sess_port}\0".encode(), addr)
                        self.say(f"hello from {addr}")
                    elif data.startswith(b"PING"):
                        self.main.sendto(b"PONG\0", addr)
                else:
                    self.session_packet(data, addr)
            now = time.time()
            for c in list(self.clients.values()):
                if now - c.last_rx > 60: self.quit(c, "timed out")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int, default=27888)
    ap.add_argument("--delay", type=int, default=1)
    ap.add_argument("--log")
    a = ap.parse_args()
    try:
        Server(a.port, a.delay, a.log).run()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
