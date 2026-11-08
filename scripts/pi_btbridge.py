#!/usr/bin/env python3
"""J606F Bluetooth bridge for a Raspberry Pi (BlueZ): Serial Port Profile server <-> TCP 127.0.0.1:7106 (loopback only).

The tablet (Windows, windrv/agent/btagent.ps1) pairs with the Pi; Windows then has an outgoing COM port to the Pi's SPP service.
The Mac reaches 127.0.0.1:7106 through "ssh -N -L 7106:127.0.0.1:7106 pi@<pi>" and runs scripts/btsh.py daemon with J606F_TCP=127.0.0.1:7106.

  pi_btbridge.py               run the bridge (SPP profile on RFCOMM channel 22, pairing agent that accepts only while --pair is active)
  pi_btbridge.py --pair 180    also discoverable + pairable for 180 s (Just Works / numeric comparison auto-accepted in that window)

Needs: python3-dbus, python3-gi, user in group "bluetooth", adapter not rfkill-blocked. No root.
"""
import os, socket, sys, threading, time
import dbus, dbus.service, dbus.mainloop.glib
from gi.repository import GLib

SPP = '00001101-0000-1000-8000-00805f9b34fb'
PORT = 7106
CHANNEL = 22
PROFILE_PATH = '/j606f/spp'
AGENT_PATH = '/j606f/agent'

state = {'bt': None, 'tcp': None, 'pair_until': 0}
lock = threading.Lock()


def log(*a):
    print(time.strftime('%H:%M:%S'), *a, flush=True)


def pump(src, dst_key, tag):
    try:
        while True:
            d = src.recv(4096)
            if not d:
                break
            with lock:
                dst = state[dst_key]
            if dst is not None:
                try:
                    dst.sendall(d)
                except OSError as e:
                    log(tag, 'send:', e)
    except OSError as e:
        log(tag, 'recv:', e)


class Profile(dbus.service.Object):
    @dbus.service.method('org.bluez.Profile1', in_signature='', out_signature='')
    def Release(self):
        log('profile released')

    @dbus.service.method('org.bluez.Profile1', in_signature='oha{sv}', out_signature='')
    def NewConnection(self, device, fd, props):
        fd = fd.take()
        s = socket.socket(fileno=fd)
        s.setblocking(True)
        log('bt connected')   # no device address in logs (rule: BT addresses are never logged)
        with lock:
            old = state['bt']
            state['bt'] = s
        if old:
            try:
                old.close()
            except OSError:
                pass

        def run():
            pump(s, 'tcp', 'bt->tcp')
            log('bt disconnected')
            with lock:
                if state['bt'] is s:
                    state['bt'] = None
            try:
                s.close()
            except OSError:
                pass
        threading.Thread(target=run, daemon=True).start()

    @dbus.service.method('org.bluez.Profile1', in_signature='o', out_signature='')
    def RequestDisconnection(self, device):
        log('disconnect requested')


class Rejected(dbus.DBusException):
    _dbus_error_name = 'org.bluez.Error.Rejected'


class Agent(dbus.service.Object):
    def _ok(self, what):
        if time.time() < state['pair_until']:
            log('pairing:', what, 'accepted')
            return
        log('pairing:', what, 'REJECTED (no --pair window)')
        raise Rejected('not in pairing window')

    @dbus.service.method('org.bluez.Agent1', in_signature='', out_signature='')
    def Release(self):
        pass

    @dbus.service.method('org.bluez.Agent1', in_signature='os', out_signature='')
    def AuthorizeService(self, device, uuid):
        if uuid.lower() == SPP:
            return
        self._ok('service ' + uuid)

    @dbus.service.method('org.bluez.Agent1', in_signature='ou', out_signature='')
    def RequestConfirmation(self, device, passkey):
        self._ok('confirmation')

    @dbus.service.method('org.bluez.Agent1', in_signature='o', out_signature='')
    def RequestAuthorization(self, device):
        self._ok('authorization')

    @dbus.service.method('org.bluez.Agent1', in_signature='o', out_signature='s')
    def RequestPinCode(self, device):
        self._ok('pin')
        return '0000'

    @dbus.service.method('org.bluez.Agent1', in_signature='o', out_signature='u')
    def RequestPasskey(self, device):
        self._ok('passkey')
        return dbus.UInt32(0)

    @dbus.service.method('org.bluez.Agent1', in_signature='ouq', out_signature='')
    def DisplayPasskey(self, device, passkey, entered):
        pass

    @dbus.service.method('org.bluez.Agent1', in_signature='os', out_signature='')
    def DisplayPinCode(self, device, pincode):
        pass

    @dbus.service.method('org.bluez.Agent1', in_signature='', out_signature='')
    def Cancel(self):
        log('pairing cancelled')


def tcp_server():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('127.0.0.1', PORT))
    srv.listen(1)
    log(f'tcp: 127.0.0.1:{PORT}')
    while True:
        c, _ = srv.accept()
        log('tcp client')
        with lock:
            old = state['tcp']
            state['tcp'] = c
        if old:
            try:
                old.close()
            except OSError:
                pass

        def run(c=c):
            pump(c, 'bt', 'tcp->bt')
            log('tcp client gone')
            with lock:
                if state['tcp'] is c:
                    state['tcp'] = None
        threading.Thread(target=run, daemon=True).start()


def main():
    pair = 0
    if '--pair' in sys.argv:
        pair = int(sys.argv[sys.argv.index('--pair') + 1])
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()
    adapter = dbus.Interface(bus.get_object('org.bluez', '/org/bluez/hci0'), 'org.freedesktop.DBus.Properties')
    adapter.Set('org.bluez.Adapter1', 'Powered', dbus.Boolean(True))
    Profile(bus, PROFILE_PATH)
    Agent(bus, AGENT_PATH)
    pm = dbus.Interface(bus.get_object('org.bluez', '/org/bluez'), 'org.bluez.ProfileManager1')
    pm.RegisterProfile(PROFILE_PATH, SPP, {'Name': 'J606F bridge', 'Role': 'server', 'Channel': dbus.UInt16(CHANNEL),
                                           'RequireAuthentication': dbus.Boolean(True), 'RequireAuthorization': dbus.Boolean(False),
                                           'AutoConnect': dbus.Boolean(False)})
    am = dbus.Interface(bus.get_object('org.bluez', '/org/bluez'), 'org.bluez.AgentManager1')
    am.RegisterAgent(AGENT_PATH, 'DisplayYesNo')
    am.RequestDefaultAgent(AGENT_PATH)
    log('SPP profile registered, channel', CHANNEL)
    if pair:
        state['pair_until'] = time.time() + pair
        adapter.Set('org.bluez.Adapter1', 'Pairable', dbus.Boolean(True))
        adapter.Set('org.bluez.Adapter1', 'DiscoverableTimeout', dbus.UInt32(pair))
        adapter.Set('org.bluez.Adapter1', 'Discoverable', dbus.Boolean(True))
        log(f'discoverable + pairable for {pair} s')

        def close_window():
            adapter.Set('org.bluez.Adapter1', 'Pairable', dbus.Boolean(False))
            adapter.Set('org.bluez.Adapter1', 'Discoverable', dbus.Boolean(False))
            log('pairing window closed')
            return False
        GLib.timeout_add_seconds(pair, close_window)
    threading.Thread(target=tcp_server, daemon=True).start()
    GLib.MainLoop().run()


if __name__ == '__main__':
    main()
