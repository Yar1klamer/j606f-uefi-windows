#!/usr/bin/env python3
"""J606F: Mac side of the Bluetooth serial agent (windrv/agent/btagent.ps1).

  btsh.py daemon                      keeps /dev/tty.Bluetooth-Incoming-Port open (Windows connects to it), serves a local unix socket
  btsh.py ping | restart | cmd "<line>" | ps "<powershell>" | put <local> <remote> | get <remote> <local>
  (cmd/ps run on the tablet with a 600 s limit; "cmd -t N <line>" / "ps -t N <line>" sets the limit to N s)

Only a unix socket in $TMPDIR (no TCP, nothing on the LAN). Output of cmd/ps goes to stdout; exit code = the remote one.
"""
import base64, os, socket, sys, termios, tty, select, time, threading

TTY = os.environ.get('J606F_TTY', '/dev/tty.Bluetooth-Incoming-Port')
TCP = os.environ.get('J606F_TCP')          # e.g. 127.0.0.1:7106 = phone bridge (android/btbridge) via "adb forward tcp:7106 tcp:7106"
SOCK = os.path.join(os.environ.get('TMPDIR', '/tmp'), 'j606f-btsh.sock')
EOT = '\x04END '


class Link:
    def __init__(self):
        self.fd = None
        self.buf = b''
        self.hello = None
        self.lock = threading.Lock()

    def open(self):
        if TCP:
            h, p = TCP.rsplit(':', 1)
            self.sock = socket.create_connection((h, int(p)), timeout=5)
            self.sock.setblocking(False)
            self.fd = self.sock.fileno()
            self.buf = b''
            return
        self.fd = os.open(TTY, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        tty.setraw(self.fd)
        a = termios.tcgetattr(self.fd)
        a[2] |= termios.CLOCAL
        termios.tcsetattr(self.fd, termios.TCSANOW, a)
        self.buf = b''

    def read_some(self, t):
        r, _, _ = select.select([self.fd], [], [], t)
        if r:
            try:
                d = os.read(self.fd, 65536)
            except BlockingIOError:
                return
            except OSError:
                self.hello = None
                raise
            if not d and TCP:
                self.hello = None
                raise OSError('bridge closed')
            self.buf += d

    def line(self, t):
        end = time.time() + t
        while b'\n' not in self.buf:
            left = end - time.time()
            if left <= 0:
                return None
            self.read_some(left)
        l, self.buf = self.buf.split(b'\n', 1)
        return l.replace(b'\0', b'').decode('utf-8', 'replace').rstrip('\r')   # NUL = agent keepalive

    def write(self, s):
        d = s.encode()
        end = time.time() + 30                  # agent not reading (hung / gone) -> the bridge's buffer fills: give up, do not hang the daemon
        while d:
            try:
                n = os.write(self.fd, d)
                d = d[n:]
            except BlockingIOError:
                if time.time() > end:
                    self.hello = None
                    raise OSError('write stalled 30 s (agent not reading)')
                time.sleep(0.01)

    def wait_hello(self, t):
        # HELLO may have been sent before we were connected (bridge drops data without a TCP client): probe with PING
        self.write('PING\n')
        end = time.time() + t
        while time.time() < end:
            l = self.line(max(0.1, end - time.time()))
            if l and (l.startswith('HELLO ') or l == 'PONG'):
                self.hello = l
                self.buf = b''
                return True
        return False

    def request(self, header, body_lines=None, timeout=600):
        """send one request, return (text, rc)"""
        # drop stale replies (late PONG of a wait_hello probe, keepalive NULs): otherwise answers come one request late
        self.buf = b''
        while select.select([self.fd], [], [], 0.05)[0]:
            self.read_some(0)
            if not self.buf:
                break
            self.buf = b''
        self.write(header + '\n')
        for b in body_lines or []:
            self.write(b + '\n')
        out = []
        end = time.time() + timeout
        while True:
            l = self.line(max(0.1, end - time.time()))
            if l is None:
                if time.time() >= end:
                    return '\n'.join(out) + '\n[timeout]', -999
                continue
            if l.startswith(EOT):
                return '\n'.join(out), int(l[len(EOT):] or 0)
            if l.startswith('HELLO '):          # agent reconnected: the request was lost
                self.hello = l
                return '\n'.join(out) + '\n[agent reconnected]', -998
            out.append(l)


def daemon():
    link = Link()
    try:
        os.unlink(SOCK)
    except FileNotFoundError:
        pass
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(SOCK)
    os.chmod(SOCK, 0o600)
    srv.listen(4)
    print(f'btsh daemon: {TCP or TTY}, socket {SOCK}', flush=True)
    last_io = time.time()
    while True:
        try:
            if link.fd is None:
                link.open()
                print('tty open, waiting for HELLO', flush=True)
            if not link.hello:
                if link.wait_hello(5):
                    print(link.hello, flush=True)
                    continue
        except OSError as e:
            print('tty:', e, flush=True)
            try:
                if TCP:
                    link.sock.close()
                else:
                    os.close(link.fd)
            except Exception:
                pass
            link.fd = None
            time.sleep(3)
            continue
        r, _, _ = select.select([srv], [], [], 1)
        if not r:
            # idle heartbeat: the agent's watchdog (btwatch.ps1) restarts it after 180 s without any received line
            if link.hello and time.time() - last_io > 20:
                last_io = time.time()
                try:
                    text, rc = link.request('PING', timeout=15)
                    if rc != 0:
                        print('idle ping:', text.strip()[-60:], rc, flush=True)
                        if rc == -999:
                            link.hello = None
                except OSError as e:
                    print('idle ping:', e, flush=True)
                    link.hello = None
            continue
        last_io = time.time()
        c, _ = srv.accept()
        with c:
            data = b''
            while not data.endswith(b'\n\0\n'):
                d = c.recv(65536)
                if not d:
                    break
                data += d
            parts = data[:-3].decode().split('\n')
            try:
                if not link.hello:
                    text, rc = 'no agent connected', -997
                else:
                    text, rc = link.request(parts[0], parts[1:])
            except OSError as e:
                text, rc = f'link error: {e}', -996
                link.hello = None
            try:
                c.sendall(f'{rc}\n{text}'.encode())
            except OSError as e:
                print('client gone:', e, flush=True)


def client(header, body=None):
    c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    c.connect(SOCK)
    c.sendall(('\n'.join([header] + (body or [])) + '\n\0\n').encode())
    data = b''
    while True:
        d = c.recv(65536)
        if not d:
            break
        data += d
    rc, _, text = data.decode('utf-8', 'replace').partition('\n')
    return text, int(rc)


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return 2
    if a[0] == 'daemon':
        daemon()
        return 0
    if a[0] == 'restart':
        text, rc = client('RESTART')
        print(text or 'restarting')
        return 0
    if a[0] == 'ping':
        text, rc = client('PING')
        print(text)
        return 0 if rc == 0 else 1
    if a[0] in ('cmd', 'ps'):
        if len(a) > 3 and a[1] == '-t':                 # btsh.py cmd -t <seconds> "<line>": time limit on the tablet (default 600 s)
            text, rc = client(('CMDT ' if a[0] == 'cmd' else 'PST ') + a[2] + ' ' + ' '.join(a[3:]))
        else:
            text, rc = client(('CMD ' if a[0] == 'cmd' else 'PS ') + ' '.join(a[1:]))
        print(text)
        return 0 if rc == 0 else (rc & 0xff or 1)
    if a[0] == 'put':
        # 256 KiB pieces: PUT for the first, PUTA (append) for the rest; a piece is written only when complete, so a failed piece is resent
        b = open(a[1], 'rb').read()
        piece = 256 * 1024
        for off in range(0, max(len(b), 1), piece):
            part = b[off:off + piece]
            s = base64.b64encode(part).decode()
            lines = [s[i:i + 76] for i in range(0, len(s), 76)] + ['.']
            for attempt in range(4):
                text, rc = client(f'{"PUT" if off == 0 else "PUTA"} {a[2]} {len(part)}', lines)
                if rc == len(part):
                    break
                print(f'piece at {off}: {text.strip()[-80:]} ({rc}), retry', file=sys.stderr)
                time.sleep(5)
            else:
                print(f'failed at {off}')
                return 1
            if len(b) > piece:
                print(f'{off + len(part)}/{len(b)}', file=sys.stderr)
        print(f'{len(b)} bytes')
        return 0
    if a[0] == 'get':
        text, rc = client(f'GET {a[1]}')
        if rc < 0:
            print(text)
            return 1
        b = base64.b64decode(''.join(text.split()))
        open(a[2], 'wb').write(b)
        print(f'{len(b)} bytes')
        return 0 if len(b) == rc else 1
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main())
