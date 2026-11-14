# Online coop S2: minimal Blaze 3 server for AO4 coop over a direct connection (no EA service).
# The game reaches it through the SDK option --xlsp_server=<this PC's IPv4> (XLSP "gos-core").
# Every request is logged decoded; unknown commands get an empty reply and an UNHANDLED line.
# Usage: python -I ao4-blaze-server.py --log <file> [--bind 0.0.0.0] [--port 42124]
import argparse
import asyncio
import ipaddress
import itertools
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import blaze_tdf as tdf  # noqa: E402
from blaze_tdf import T_INT, T_STRING, T_STRUCT, T_UNION, T_LIST, T_MAP  # noqa: E402

COMPONENTS = {1: 'Authentication', 4: 'GameManager', 5: 'Redirector', 6: 'Playgroups', 7: 'Stats',
              9: 'Util', 10: 'CensusData', 11: 'Clubs', 15: 'Messaging', 20: 'Locker', 21: 'Rooms',
              25: 'Association', 28: 'GameReporting', 0x7802: 'UserSessions'}
COMPONENT_NAMES = {**COMPONENTS, 0x0802: 'AO2'}
XLSP_SERVICE_ID = 0x45410805  # service id the title passes to XNetServerToInAddr

args = None
log_file = None
user_ids = itertools.count(1001)
game_ids = itertools.count(1)
list_ids = itertools.count(1)
games = {}  # game id -> Game


def log(text):
    line = time.strftime('%H:%M:%S ') + text
    print(line, flush=True)
    if log_file:
        log_file.write(line + '\n')
        log_file.flush()


def field(members, tag, default=None):
    for t, _, v in members:
        if t == tag:
            return v
    return default


class Session:
    def __init__(self, reader, writer):
        self.reader, self.writer = reader, writer
        self.peer = writer.get_extra_info('peername')
        self.user_id = next(user_ids)
        self.xuid = 0
        self.gamertag = 'User'
        self.xnaddr = b''
        self.after_reply = []  # notifications to send once the current reply is out
        self.game = None
        self.retired_game_ids = set()  # acknowledge late self-leave after GameRemoved
        self.game_lists = set()

    async def run(self):
        if not peer_allowed(self.peer):
            # LAN/VPN mode: the server has no authentication, so only listed networks may talk to it.
            log(f'[{self.peer}] rejected (not in --allow)')
            self.writer.close()
            return
        log(f'[{self.peer}] connected (user id {self.user_id})')
        try:
            while True:
                head = await self.reader.readexactly(12)
                length, component, command, error, flags, msg_id = tdf.parse_header(head)
                body = await self.reader.readexactly(length)
                await self.handle(component, command, flags, msg_id, body)
        except (asyncio.IncompleteReadError, ConnectionError) as exc:
            log(f'[{self.peer}] closed ({exc.__class__.__name__})')
        game = self.game
        if game and self in game.players:
            game.players.remove(self)
            log(f'GAME {game.gid}: {self.gamertag} disconnected')
            if game.host is self:
                # Coop has no host migration: the remaining partner must leave the game cleanly
                # (a host-less game made the guest crash on a null access).
                remove_game(game, REASON_HOST_LEAVING)
            else:
                game.broadcast(NOTIFY_PLAYER_REMOVED, [('CNTX', T_INT, 0), ('GID', T_INT, game.gid),
                                                       ('PID', T_INT, self.user_id), ('REAS', T_INT, 0)])
                if not game.players:
                    games.pop(game.gid, None)

    def send(self, component, command, members, msg_type, msg_id=0, error=0):
        body = tdf.encode(members)
        self.writer.write(tdf.frame(component, command, body, msg_type, msg_id, error))

    def notify(self, component, command, members):
        self.send(component, command, members, tdf.MSG_NOTIFICATION)
        name = COMPONENT_NAMES.get(component, f'0x{component:04X}')
        log(f'[{self.peer}] NOTIFY {name} cmd={command}\n' +
            '\n'.join('    ' + l for l in tdf.pretty(tdf.decode(tdf.encode(members)))))

    async def handle(self, component, command, flags, msg_id, body):
        name = COMPONENT_NAMES.get(component, f'0x{component:04X}')
        try:
            members = tdf.decode(body)
            decoded = '\n'.join('    ' + l for l in tdf.pretty(members))
        except Exception as exc:  # keep serving even if a request does not decode
            members, decoded = [], f'    <decode error {exc}> {body.hex()}'
        log(f'[{self.peer}] REQ {name}({component}) cmd={command} id={msg_id} flags={flags:04X} len={len(body)}\n{decoded}')
        handler = HANDLERS.get((component, command))
        if handler is None:
            log(f'[{self.peer}] UNHANDLED {name}({component}) cmd={command} -> empty reply')
            reply = []
        else:
            reply = handler(self, members)
        self.send(component, command, reply, tdf.MSG_REPLY, msg_id)
        if reply:
            log(f'[{self.peer}] REPLY {name} cmd={command}\n' + '\n'.join('    ' + l for l in tdf.pretty(tdf.decode(tdf.encode(reply)))))
        pending, self.after_reply = self.after_reply, []
        for action in pending:
            action()
        await self.writer.drain()


# --- Redirector -----------------------------------------------------------------------------
def redirector_get_server_instance(session, req):
    fpid = field(req, 'FPID')
    if isinstance(fpid, tuple) and len(fpid) == 4 and isinstance(fpid[3], list):
        session.gamertag = field(fpid[3], 'GTAG', 'User')
        session.xuid = field(fpid[3], 'XUID', 0)
    # ServerAddress union, member 1 = XboxServerAddress {PORT, SID, SITE}: the Blaze server is
    # this same process, reached through the same XLSP service.
    address = [('PORT', T_INT, args.port), ('SID', T_INT, XLSP_SERVICE_ID), ('SITE', T_STRING, 'ao4-coop')]
    return [('ADDR', T_UNION, (1, ('VALU', T_STRUCT, address))),
            ('SECU', T_INT, 0),
            ('XDNS', T_INT, 0)]


# --- Util -----------------------------------------------------------------------------------
def util_pre_auth(session, req):
    ping_site = [('PSA', T_STRING, args.public_host), ('PSP', T_INT, args.port), ('SNA', T_STRING, 'ao4-coop')]
    qos = [('BWPS', T_STRUCT, ping_site),
           ('LNP', T_INT, 10),
           ('LTPS', T_MAP, (T_STRING, T_STRUCT, [('ao4-coop', ping_site)])),
           ('SVID', T_INT, XLSP_SERVICE_ID)]
    # xlspConnectionIdleTimeout (s) is DirtySock's XLSP lookup TTL ('xtim'). While the lookup lives, NetConn's
    # catch-all address map {0/0 -> XLSP} also redirects peer VDP traffic to this server, so keep it short.
    config = [('pingPeriod', '15s'), ('voipHeadsetUpdateRate', '1000'),
              ('xlspConnectionIdleTimeout', str(args.xlsp_idle_timeout))]
    return [('ASRC', T_STRING, '303107'),
            ('CIDS', T_LIST, (T_INT, sorted(COMPONENTS))),
            ('CONF', T_STRUCT, [('CONF', T_MAP, (T_STRING, T_STRING, config))]),
            ('INST', T_STRING, 'armyoftwo-3-xbl2'),
            ('MINR', T_INT, 0),
            ('NASP', T_STRING, 'cem_ea_id'),
            ('PILD', T_STRING, ''),
            ('PLAT', T_STRING, 'xenon'),
            ('QOSS', T_STRUCT, qos),
            ('RSRC', T_STRING, '303107'),
            ('SVER', T_STRING, 'Blaze 3.15.08.0 (AO4 coop mini server)')]


def util_ping(session, req):
    return [('STIM', T_INT, int(time.time()))]


def util_post_auth(session, req):
    tele = [('ADRS', T_STRING, ''), ('ANON', T_INT, 0), ('DISA', T_STRING, ''), ('EDCT', T_INT, 0),
            ('FILT', T_STRING, ''), ('LOC', T_INT, 1684362579), ('MINR', T_INT, 0), ('NOOK', T_STRING, ''),
            ('PORT', T_INT, 0), ('SDLY', T_INT, 15000), ('SESS', T_STRING, 'ao4'), ('SKEY', T_STRING, ''),
            ('SPCT', T_INT, 0), ('STIM', T_STRING, ''), ('SVNM', T_STRING, 'telemetry-3-common')]
    tick = [('ADRS', T_STRING, ''), ('PORT', T_INT, 0), ('SKEY', T_STRING, '')]
    pss = [('ADRS', T_STRING, ''), ('OIDS', T_LIST, (T_STRING, [])), ('PJID', T_STRING, ''), ('PORT', T_INT, 0),
           ('TIID', T_INT, 0)]
    return [('PSS', T_STRUCT, pss), ('TELE', T_STRUCT, tele), ('TICK', T_STRUCT, tick),
            ('UROP', T_STRUCT, [('TMOP', T_INT, 1), ('UID', T_INT, session.user_id)])]


# --- Authentication / user session ------------------------------------------------------------
def persona(session):
    return [('DSNM', T_STRING, session.gamertag), ('LAST', T_INT, int(time.time())),
            ('PID', T_INT, session.user_id), ('PLAT', T_INT, 0)]


def session_info(session):
    return [('BUID', T_INT, session.user_id), ('FRST', T_INT, 0), ('KEY', T_STRING, f'ao4key{session.user_id}'),
            ('LLOG', T_INT, int(time.time())), ('MAIL', T_STRING, f'{session.user_id}@ao4.coop'),
            ('PDTL', T_STRUCT, persona(session)), ('UID', T_INT, session.user_id)]


def user_identification(session):
    return [('AID', T_INT, session.user_id), ('ALOC', T_INT, 1684362579), ('EXBB', tdf.T_BLOB, b''),
            ('EXID', T_INT, session.xuid), ('ID', T_INT, session.user_id), ('NAME', T_STRING, session.gamertag),
            ('ORIG', T_INT, session.xuid)]


def extended_data(session):
    xboxaddr = [('XDDR', tdf.T_BLOB, session.xnaddr), ('XUID', T_INT, session.xuid)]
    return [('ADDR', T_UNION, (0, ('VALU', T_STRUCT, xboxaddr)) if session.xnaddr else None),
            ('BPS', T_STRING, 'ao4-coop'), ('CMAP', T_MAP, (T_INT, T_INT, [])), ('CTY', T_STRING, ''),
            ('DMAP', T_MAP, (T_INT, T_INT, [])), ('HWFG', T_INT, 0), ('PSLM', T_LIST, (T_INT, [])),
            ('QDAT', T_STRUCT, [('DBPS', T_INT, 0), ('NATT', T_INT, 0), ('UBPS', T_INT, 0)]),
            ('UATT', T_INT, 0), ('ULST', T_LIST, (tdf.T_TRIPLE, []))]


def notify_logged_in(session):
    session.notify(0x7802, 2, [('DATA', T_STRUCT, extended_data(session)), ('USER', T_STRUCT, user_identification(session))])
    session.notify(0x7802, 1, [('DATA', T_STRUCT, extended_data(session)), ('SUBS', T_INT, 1), ('USID', T_INT, session.user_id)])
    login = [('ALOC', T_INT, 1684362579), ('BUID', T_INT, session.user_id), ('DSNM', T_STRING, session.gamertag),
             ('FRST', T_INT, 0), ('KEY', T_STRING, f'ao4key{session.user_id}'), ('LAST', T_INT, int(time.time())),
             ('LLOG', T_INT, int(time.time())), ('MAIL', T_STRING, f'{session.user_id}@ao4.coop'),
             ('PID', T_INT, session.user_id), ('PLAT', T_INT, 0), ('UID', T_INT, session.user_id),
             ('XREF', T_INT, session.xuid)]
    session.notify(0x7802, 8, login)


def auth_xbox_login(session, req):
    session.gamertag = field(req, 'GTAG', session.gamertag) or session.gamertag
    session.xuid = field(req, 'XUID', session.xuid)
    session.after_reply.append(lambda: notify_logged_in(session))
    return [('AGUP', T_INT, 0), ('ANON', T_INT, 0), ('NTOS', T_INT, 0),
            ('SESS', T_STRUCT, session_info(session)), ('SPAM', T_INT, 0), ('UNDR', T_INT, 0)]


def user_sessions_update_network_info(session, req):
    addr = field(req, 'ADDR')
    if isinstance(addr, tuple) and len(addr) == 4 and isinstance(addr[3], list):
        session.xnaddr = field(addr[3], 'XDDR', b'') or b''
    return []


# --- Online Pass ------------------------------------------------------------------------------
# Persistence/Entitlements/GameQueries: the redeemed Online Pass is tag PROJECT10_CODE_CONSUMED
# (license olp_free) in group AO4360, project 129570. EntitlementType ONLINE_ACCESS = 1,
# status ACTIVE = 1.
ONLINE_PASS_TAGS = ['PROJECT10_CODE_CONSUMED', 'PROJECT10_CODE_CONSUMED_LE1', 'PROJECT10_CODE_PURCHASE', 'LIMITEDEDITION']


def auth_list_user_entitlements2(session, req):
    groups = field(req, 'GNLS', []) or []
    wanted = field(req, 'ETAG', '')
    entitlements = []
    if 'AO4360' in groups:
        for index, tag in enumerate(ONLINE_PASS_TAGS):
            if wanted and wanted != tag:
                continue
            entitlements.append([
                ('DEVI', T_STRING, ''), ('GDAY', T_STRING, '2013-03-26T00:00Z'), ('GNAM', T_STRING, 'AO4360'),
                ('ID', T_INT, 900000 + session.user_id * 10 + index), ('ISCO', T_INT, 0),
                ('PID', T_INT, session.user_id), ('PJID', T_STRING, '129570'), ('PRCA', T_INT, 0),
                ('PRID', T_STRING, ''), ('STAT', T_INT, 1), ('STRC', T_INT, 1), ('TAG', T_STRING, tag),
                ('TDAY', T_STRING, ''), ('TYPE', T_INT, 1), ('UCNT', T_INT, 0), ('VER', T_INT, 0)])
    return [('NLST', T_LIST, (T_STRUCT, entitlements))]


def ao2_get_online_pass_config(session, req):
    passes = [[('LICE', T_STRING, lic), ('SKUI', T_STRING, '')] for lic in ('olp_free', 'olp_le_free', 'olp_purchase', 'le')]
    return [('CFG', T_STRUCT, [('OLPS', T_LIST, (T_STRUCT, passes)), ('TITI', T_INT, 0)]), ('SUCC', T_INT, 1)]


def ao2_check_online_pass_kill_switch(session, req):
    # EA switched the Online Pass requirement off in 2013 through this kill switch; ACTI = 1
    # reproduces that final server state, so no pass is demanded.
    return [('ACTI', T_INT, 1)]


# --- GameManager ------------------------------------------------------------------------------
# Enums (read from the running client): game state INITIALIZING 1, PRE_GAME 130, IN_GAME 131;
# player state ACTIVE_CONNECTING 2, ACTIVE_CONNECTED 4; setup context CREATE 0, JOIN 1.
GM = 4
GSTA_INITIALIZING, GSTA_PRE_GAME, GSTA_IN_GAME = 1, 130, 131
PLAYER_CONNECTING, PLAYER_CONNECTED = 2, 4
NOTIFY_GAME_SETUP, NOTIFY_PLAYER_JOINING, NOTIFY_PLAYER_JOIN_COMPLETED = 20, 21, 30
NOTIFY_PLAYER_REMOVED, NOTIFY_PLATFORM_HOST_INITIALIZED = 40, 71
NOTIFY_GAME_REMOVED = 16
REASON_HOST_LEAVING = 3  # GameDestructionReason, read from the title's enum table
NOTIFY_GAME_STATE_CHANGE, NOTIFY_GAME_SESSION_UPDATED, NOTIFY_PLAYER_STATE_CHANGE = 100, 115, 116
NOTIFY_GAME_ATTRIB_CHANGE, NOTIFY_PLAYER_ATTRIB_CHANGE, NOTIFY_GAME_SETTINGS_CHANGE = 80, 90, 110


class Game:
    def __init__(self, host, req):
        self.gid = next(game_ids)
        self.host = host
        self.players = [host]  # slot order; host is slot 0
        self.state = GSTA_INITIALIZING
        self.attr = dict(field(req, 'ATTR', {}) or {})
        self.name = field(req, 'GNAM', host.gamertag) or host.gamertag
        self.settings = field(req, 'GSET', 0)
        self.game_type = field(req, 'GTYP', '')
        self.host_net = field(req, 'HNET', []) or []
        self.topology = field(req, 'NTOP', 0)
        self.presence = field(req, 'PRES', 0)
        self.voip = field(req, 'VOIP', 0)
        self.version = field(req, 'VSTR', '')
        self.capacity = field(req, 'PCAP', [2, 0]) or [2, 0]
        self.max_players = field(req, 'PMAX', 2) or 2
        self.xnnc = b''
        self.xses = b''
        self.player_state = {host.user_id: PLAYER_CONNECTING}
        self.player_attributes = {}

    def host_info(self):
        return [('HPID', T_INT, self.host.user_id), ('HSLT', T_INT, 0)]

    def data(self):
        return [('ADMN', T_LIST, (T_INT, [self.host.user_id])),
                ('ATTR', T_MAP, (T_STRING, T_STRING, sorted(self.attr.items()))),
                ('CAP', T_LIST, (T_INT, list(self.capacity))),
                ('CRIT', T_MAP, (T_STRING, T_STRING, [])),
                ('GID', T_INT, self.gid), ('GNAM', T_STRING, self.name), ('GPVH', T_INT, 0),
                ('GSET', T_INT, self.settings), ('GSID', T_INT, self.gid), ('GSTA', T_INT, self.state),
                ('GTYP', T_STRING, self.game_type), ('GURL', T_STRING, ''),
                ('HNET', T_LIST, (T_STRUCT, list(self.host_net))),
                ('HSES', T_INT, self.host.user_id), ('IGNO', T_INT, 0),
                ('MATR', T_MAP, (T_STRING, T_STRING, [])), ('MCAP', T_INT, self.max_players),
                ('NQOS', T_STRUCT, [('DBPS', T_INT, 0), ('NATT', T_INT, 0), ('UBPS', T_INT, 0)]),
                ('NRES', T_INT, 0), ('NTOP', T_INT, self.topology), ('PGID', T_STRING, ''), ('PGSR', tdf.T_BLOB, b''),
                ('PHST', T_STRUCT, self.host_info()), ('PRES', T_INT, self.presence), ('PSAS', T_STRING, 'ao4-coop'),
                ('QCAP', T_INT, 0), ('SEED', T_INT, 0x4F4A), ('TCAP', T_INT, 0),
                ('THST', T_STRUCT, self.host_info()), ('TIDS', T_LIST, (T_INT, [])), ('UUID', T_STRING, f'ao4-{self.gid}'),
                ('VOIP', T_INT, self.voip), ('VSTR', T_STRING, self.version),
                ('XNNC', tdf.T_BLOB, self.xnnc), ('XSES', tdf.T_BLOB, self.xses)]

    def player(self, session):
        slot = self.players.index(session)
        pnet = (0, ('VALU', T_STRUCT, [('XDDR', tdf.T_BLOB, session.xnaddr), ('XUID', T_INT, session.xuid)]))
        return [('BLOB', tdf.T_BLOB, b''), ('EXID', T_INT, session.xuid), ('GID', T_INT, self.gid),
                ('LOC', T_INT, 1684362579), ('NAME', T_STRING, session.gamertag),
                ('PATT', T_MAP, (T_STRING, T_STRING, sorted(self.player_attributes.get(session.user_id, {}).items()))), ('PID', T_INT, session.user_id),
                ('PNET', T_UNION, pnet), ('SID', T_INT, slot), ('SLOT', T_INT, 0),
                ('STAT', T_INT, self.player_state.get(session.user_id, PLAYER_CONNECTING)),
                ('TIDX', T_INT, 65535), ('TIME', T_INT, int(time.time() * 1000000)),
                ('UGID', tdf.T_TRIPLE, (0, 0, 0)), ('UID', T_INT, session.user_id)]

    def setup(self, session, context):
        return [('GAME', T_STRUCT, self.data()),
                ('PROS', T_LIST, (T_STRUCT, [self.player(p) for p in self.players])),
                ('REAS', T_UNION, (0, ('VALU', T_STRUCT, [('DCTX', T_INT, context)])))]

    def broadcast(self, command, members, skip=None):
        for p in self.players:
            if p is not skip:
                p.notify(GM, command, members)


def gm_create_game(session, req):
    game = Game(session, req)
    games[game.gid] = game
    session.game = game
    log(f'GAME {game.gid} created by {session.gamertag} ({session.user_id}) attr={game.attr}')
    session.after_reply.append(lambda: session.notify(GM, NOTIFY_GAME_SETUP, game.setup(session, 0)))
    return [('GID', T_INT, game.gid)]


def find_game(req):
    gid = field(req, 'GID', 0)
    return games.get(gid) or (next(reversed(games.values())) if games and not gid else None)


def gm_advance_game_state(session, req):
    game = find_game(req)
    if game:
        game.state = field(req, 'GSTA', game.state)
        log(f'GAME {game.gid} state -> {game.state}')
        session.after_reply.append(lambda: game.broadcast(
            NOTIFY_GAME_STATE_CHANGE, [('GID', T_INT, game.gid), ('GSTA', T_INT, game.state)]))
    return []


def gm_update_game_session(session, req):
    game = find_game(req)
    if game:
        game.xnnc = field(req, 'XNNC', b'') or b''
        game.xses = field(req, 'XSES', b'') or b''
        log(f'GAME {game.gid} session xnnc={game.xnnc.hex()} xses={game.xses.hex()}')
        session.after_reply.append(lambda: game.broadcast(
            NOTIFY_GAME_SESSION_UPDATED,
            [('GID', T_INT, game.gid), ('XNNC', tdf.T_BLOB, game.xnnc), ('XSES', tdf.T_BLOB, game.xses)]))
    return []


def gm_finalize_game_creation(session, req):
    game = find_game(req)
    if game:
        # This title sends XSES/XNNC here, before it ever calls updateGameSession.
        # Keep the opaque title serialization unchanged for the joining peer.
        game.xses = field(req, 'XSES', game.xses) or b''
        game.xnnc = field(req, 'XNNC', game.xnnc) or b''
        log(f'GAME {game.gid} finalized xses_bytes={len(game.xses)} xnnc_bytes={len(game.xnnc)}')
        session.after_reply.append(lambda: session.notify(
            GM, NOTIFY_PLATFORM_HOST_INITIALIZED, [('GID', T_INT, game.gid), ('PHST', T_INT, 0)]))
    return []


def gm_set_player_attributes(session, req):
    game = find_game(req)
    if game and session in game.players:
        pid = field(req, 'PID', session.user_id)
        if pid == session.user_id:
            changed = field(req, 'ATTR', {}) or {}
            game.player_attributes.setdefault(pid, {}).update(changed)
            log(f'GAME {game.gid} player {pid} attr {changed}')
            # Every member (sender included) learns the change, e.g. the lobby 'Ready' flag.
            session.after_reply.append(lambda: game.broadcast(NOTIFY_PLAYER_ATTRIB_CHANGE, [
                ('ATTR', T_MAP, (T_STRING, T_STRING, sorted(changed.items()))),
                ('GID', T_INT, game.gid), ('PID', T_INT, pid)]))
    return []


def gm_set_game_attributes(session, req):
    game = find_game(req)
    if game:
        changed = field(req, 'ATTR', {}) or {}
        game.attr.update(changed)
        log(f'GAME {game.gid} attr {changed}')
        session.after_reply.append(lambda: game.broadcast(NOTIFY_GAME_ATTRIB_CHANGE, [
            ('ATTR', T_MAP, (T_STRING, T_STRING, sorted(changed.items()))), ('GID', T_INT, game.gid)]))
    return []


def gm_set_game_settings(session, req):
    game = find_game(req)
    if game:
        game.settings = field(req, 'GSET', game.settings)
        log(f'GAME {game.gid} settings -> {game.settings}')
        session.after_reply.append(lambda: game.broadcast(NOTIFY_GAME_SETTINGS_CHANGE, [
            ('ATTR', T_INT, game.settings), ('GID', T_INT, game.gid)]))
    return []


def browser_game(game):
    roster = []
    for player in game.players:
        roster.append([('EXID', T_INT, player.xuid), ('LOC', T_INT, 1684362579),
                       ('NAME', T_STRING, player.gamertag),
                       ('PATT', T_MAP, (T_STRING, T_STRING, sorted(game.player_attributes.get(player.user_id, {}).items()))),
                       ('PID', T_INT, player.user_id), ('STAT', T_INT, game.player_state[player.user_id]),
                       ('TIDX', T_INT, 65535)])
    return [('ADMN', T_LIST, (T_INT, [game.host.user_id])),
            ('ATTR', T_MAP, (T_STRING, T_STRING, sorted(game.attr.items()))),
            ('CAP', T_LIST, (T_INT, list(game.capacity))), ('CRIT', T_MAP, (T_STRING, T_STRING, [])),
            ('GID', T_INT, game.gid), ('GNAM', T_STRING, game.name), ('GSET', T_INT, game.settings),
            ('GSTA', T_INT, game.state), ('GURL', T_STRING, ''),
            ('HNET', T_LIST, (T_STRUCT, list(game.host_net))), ('HOST', T_INT, game.host.user_id),
            ('NTOP', T_INT, game.topology), ('PCNT', T_LIST, (T_INT, [len(game.players), 0])),
            ('PRES', T_INT, game.presence), ('PSAS', T_STRING, 'ao4-coop'),
            ('PSID', T_STRING, ''), ('QCAP', T_INT, 0), ('QCNT', T_INT, 0),
            ('ROST', T_LIST, (T_STRUCT, roster)), ('SID', T_INT, game.gid),
            ('TCAP', T_INT, 0), ('TINF', T_LIST, (T_STRUCT, [])),
            ('VOIP', T_INT, game.voip), ('VSTR', T_STRING, game.version)]


def gm_get_game_list(session, req):
    list_id = next(list_ids)
    session.game_lists.add(list_id)
    version = field(req, 'GVER', '')
    available = [g for g in games.values() if g.host is not session and len(g.players) < g.max_players
                 and (not version or g.version == version)]
    available = available[:max(0, min(int(field(req, 'LCAP', 10)), 100))]
    # Runtime TDF tables: GetGameListResponse(GLID/MAXF/NGD),
    # NotifyGameListUpdate(DONE/GLID/REMV/UPDT), GameBrowserMatchData(FIT/GAM).
    updates = [[('FIT', T_INT, 1000), ('GAM', T_STRUCT, browser_game(g))] for g in available]
    log(f'BROWSER list={list_id} viewer={session.gamertag} games={[g.gid for g in available]}')
    session.after_reply.append(lambda: session.notify(GM, 201,
        [('DONE', T_INT, 1), ('GLID', T_INT, list_id), ('REMV', T_LIST, (T_INT, [])),
         ('UPDT', T_LIST, (T_STRUCT, updates))]))
    return [('GLID', T_INT, list_id), ('MAXF', T_INT, 1000), ('NGD', T_INT, len(available))]


def gm_destroy_game_list(session, req):
    session.game_lists.discard(field(req, 'GLID', 0))
    return []


def gm_update_mesh_connection(session, req):
    # TARG: PlayerConnectionStatus list (PID, STAT CONNECTED = 2). A guest that reports its
    # connection to the host is complete; the host reporting the guest likewise.
    game = find_game(req)
    if not game:
        return []
    for target in field(req, 'TARG', []) or []:
        members = target if isinstance(target, list) else []
        pid, stat = field(members, 'PID', 0), field(members, 'STAT', 0)
        log(f'GAME {game.gid} mesh {session.user_id} -> {pid} stat={stat}')
        if stat == 2:
            for joined in (session.user_id, pid):
                if game.player_state.get(joined) != PLAYER_CONNECTED and joined in game.player_state:
                    game.player_state[joined] = PLAYER_CONNECTED
                    session.after_reply.append(lambda j=joined: game.broadcast(
                        NOTIFY_PLAYER_JOIN_COMPLETED, [('GID', T_INT, game.gid), ('PID', T_INT, j)]))
    return []


def gm_join_game(session, req):
    game = find_game(req)
    if not game or len(game.players) >= game.max_players:
        log(f'JOIN by {session.gamertag}: no open game (games={list(games)})')
        return [('GID', T_INT, 0), ('JGS', T_INT, 0)]
    game.players.append(session)
    game.player_state[session.user_id] = PLAYER_CONNECTING
    session.game = game
    log(f'GAME {game.gid} joined by {session.gamertag} ({session.user_id})')

    def announce():
        session.notify(GM, NOTIFY_GAME_SETUP, game.setup(session, 1))
        game.broadcast(NOTIFY_PLAYER_JOINING, [('GID', T_INT, game.gid), ('PDAT', T_STRUCT, game.player(session))],
                       skip=session)
    session.after_reply.append(announce)
    return [('GID', T_INT, game.gid), ('JGS', T_INT, 0)]


def gm_remove_player(session, req):
    game = find_game(req)
    gid = field(req, 'GID', 0)
    pid = field(req, 'PID', session.user_id)
    if game is None and gid in session.retired_game_ids and pid == session.user_id:
        # The title sends removePlayer after processing NotifyGameRemoved. It still
        # waits for PlayerRemoved to complete local teardown before another join.
        session.after_reply.append(lambda: session.notify(GM, NOTIFY_PLAYER_REMOVED,
            [('CNTX', T_INT, field(req, 'CNTX', 0)), ('GID', T_INT, gid),
             ('PID', T_INT, pid), ('REAS', T_INT, field(req, 'REAS', 6))]))
        log(f'GAME {gid} late self-leave completed for {pid}')
        return []
    if game:
        leaving = [p for p in game.players if p.user_id == pid]
        recipients = tuple(game.players)
        def announce_leave():
            for recipient in recipients:
                recipient.notify(GM, NOTIFY_PLAYER_REMOVED,
                    [('CNTX', T_INT, field(req, 'CNTX', 0)), ('GID', T_INT, game.gid),
                     ('PID', T_INT, pid), ('REAS', T_INT, field(req, 'REAS', 6))])
        if leaving:
            session.after_reply.append(announce_leave)
        for p in leaving:
            game.players.remove(p)
            game.player_state.pop(p.user_id, None)
            game.player_attributes.pop(p.user_id, None)
            if p.game is game:
                p.game = None
        if not game.players:
            games.pop(game.gid, None)
    return []


def remove_game(game, reason, skip=None):
    games.pop(game.gid, None)
    log(f'GAME {game.gid} removed reason={reason}')
    for member in tuple(game.players):
        if member is not skip:
            # Membership teardown must complete even in a lobby without a peer
            # disconnect. PlayerRemovedReason.GAME_DESTROYED=4 (title enum table).
            member.notify(GM, NOTIFY_PLAYER_REMOVED,
                [('CNTX', T_INT, 0), ('GID', T_INT, game.gid),
                 ('PID', T_INT, member.user_id), ('REAS', T_INT, 4)])
            member.notify(GM, NOTIFY_GAME_REMOVED,
                [('GID', T_INT, game.gid), ('REAS', T_INT, reason)])
    for member in game.players:
        member.retired_game_ids.add(game.gid)
        if len(member.retired_game_ids) > 16:
            member.retired_game_ids.remove(min(member.retired_game_ids))
        if member.game is game:
            member.game = None
    game.players.clear()
    game.player_state.clear()
    game.player_attributes.clear()


def gm_destroy_game(session, req):
    game = find_game(req)
    if game:
        reason = REASON_HOST_LEAVING if game.host is session else field(req, 'REAS', 0)
        session.after_reply.append(lambda: remove_game(game, reason, skip=session))
    return [('GID', T_INT, game.gid if game else 0)]


HANDLERS = {
    (GM, 1): gm_create_game,
    (GM, 2): gm_destroy_game,
    (GM, 3): gm_advance_game_state,
    (GM, 4): gm_set_game_settings,
    (GM, 7): gm_set_game_attributes,
    (GM, 8): gm_set_player_attributes,
    (GM, 9): gm_join_game,
    (GM, 11): gm_remove_player,
    (GM, 15): gm_finalize_game_creation,
    (GM, 26): gm_update_game_session,
    (GM, 29): gm_update_mesh_connection,
    (GM, 100): gm_get_game_list,
    (GM, 101): gm_get_game_list,
    (GM, 102): gm_destroy_game_list,
    (1, 29): auth_list_user_entitlements2,
    (0x0802, 17): ao2_check_online_pass_kill_switch,
    (0x0802, 21): ao2_get_online_pass_config,
    (5, 1): redirector_get_server_instance,
    (9, 2): util_ping,
    (9, 7): util_pre_auth,
    (9, 8): util_post_auth,
    (1, 170): auth_xbox_login,
    (0x7802, 20): user_sessions_update_network_info,
}


allowed_networks = [ipaddress.ip_network('127.0.0.0/8')]


def peer_allowed(peer):
    try:
        address = ipaddress.ip_address(peer[0])
    except (TypeError, ValueError, IndexError):
        return False
    return any(address in network for network in allowed_networks)


async def main():
    global args, log_file, allowed_networks
    parser = argparse.ArgumentParser()
    parser.add_argument('--bind', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=42124)
    parser.add_argument('--public-host', default='127.0.0.1', help='address announced to clients')
    parser.add_argument('--log', required=True)
    parser.add_argument('--seconds', type=int, default=0, help='stop after this many seconds (0 = run)')
    parser.add_argument('--xlsp-idle-timeout', type=int, default=10,
                        help='seconds; old value 300 kept peer traffic on the server for 5 minutes')
    parser.add_argument('--allow', default='127.0.0.0/8',
                        help='comma-separated client networks (CIDR); default loopback only')
    args = parser.parse_args()
    allowed_networks = [ipaddress.ip_network(n.strip(), strict=False) for n in args.allow.split(',') if n.strip()]
    log_file = open(args.log, 'a', encoding='utf-8')
    server = await asyncio.start_server(lambda r, w: Session(r, w).run(), args.bind, args.port)
    log(f'AO4 mini Blaze server on {args.bind}:{args.port} (public {args.public_host}, allow {args.allow})')
    async with server:
        if args.seconds:
            await asyncio.sleep(args.seconds)
        else:
            await server.serve_forever()
    log('stop')


if __name__ == '__main__':
    asyncio.run(main())
