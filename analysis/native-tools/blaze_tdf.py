# Online coop S2: Blaze 3 "Fire" frame and TDF codec (EA BlazeSDK 3.x as used by AO4 on Xenon).
# Frame: u16 body length, u16 component, u16 command, u16 error, u16 message type/flags, u16 id.
# TDF member: 3-byte packed tag (four 6-bit chars + 0x20), 1 type byte, value.
import struct

T_INT, T_STRING, T_BLOB, T_STRUCT, T_LIST, T_MAP, T_UNION, T_INTLIST, T_PAIR, T_TRIPLE, T_FLOAT = (
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10)

# Message types in the upper bits of the flags word.
MSG_MESSAGE, MSG_REPLY, MSG_NOTIFICATION, MSG_ERROR_REPLY = 0x0000, 0x1000, 0x2000, 0x3000


def tag_bytes(tag):
    tag = tag.ljust(4)
    v = 0
    for ch in tag:
        v = (v << 6) | ((ord(ch) - 0x20) & 0x3F)
    return bytes([(v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF])


def tag_name(b):
    v = (b[0] << 16) | (b[1] << 8) | b[2]
    return ''.join(chr(((v >> (18 - 6 * i)) & 0x3F) + 0x20) for i in range(4)).rstrip()


def enc_varint(value):
    neg = value < 0
    value = -value if neg else value
    first = value & 0x3F
    value >>= 6
    out = bytearray([first | (0x40 if neg else 0) | (0x80 if value else 0)])
    while value:
        byte = value & 0x7F
        value >>= 7
        out.append(byte | (0x80 if value else 0))
    return bytes(out)


def dec_varint(data, pos):
    b = data[pos]
    pos += 1
    neg = bool(b & 0x40)
    value = b & 0x3F
    shift = 6
    more = b & 0x80
    while more:
        b = data[pos]
        pos += 1
        value |= (b & 0x7F) << shift
        shift += 7
        more = b & 0x80
    return (-value if neg else value), pos


class UnionItem(tuple):
    """List item of a union list: (active member index, struct members or None)."""

    def __new__(cls, active, members):
        return super().__new__(cls, (active, members))


class Reader:
    def __init__(self, data):
        self.data, self.pos = data, 0

    def value(self, typ):
        d = self.data
        if typ == T_INT:
            v, self.pos = dec_varint(d, self.pos)
            return v
        if typ == T_STRING:
            n, self.pos = dec_varint(d, self.pos)
            s = d[self.pos:self.pos + n]
            self.pos += n
            return s.rstrip(b'\0').decode('utf-8', 'replace')
        if typ == T_BLOB:
            n, self.pos = dec_varint(d, self.pos)
            s = d[self.pos:self.pos + n]
            self.pos += n
            return bytes(s)
        if typ == T_STRUCT:
            return self.members(struct_end=True)
        if typ == T_LIST:
            sub = d[self.pos]
            self.pos += 1
            n, self.pos = dec_varint(d, self.pos)
            if sub == T_STRUCT and n and d[self.pos] < 0x80:
                # List of unions (e.g. HNET network addresses): each item is the active member
                # index followed by that member's struct fields (no VALU tag).
                items = []
                for _ in range(n):
                    active = d[self.pos]
                    self.pos += 1
                    items.append(UnionItem(active, None if active == 0x7F else self.members(struct_end=True)))
                return items
            return [self.value(sub) for _ in range(n)]
        if typ == T_MAP:
            kt, vt = d[self.pos], d[self.pos + 1]
            self.pos += 2
            n, self.pos = dec_varint(d, self.pos)
            out = {}
            for _ in range(n):
                k = self.value(kt)
                out[k if not isinstance(k, (dict, list)) else repr(k)] = self.value(vt)
            return out
        if typ == T_UNION:
            active = d[self.pos]
            self.pos += 1
            if active == 0x7F:
                return ('union', None)
            tag = tag_name(d[self.pos:self.pos + 3])
            t = d[self.pos + 3]
            self.pos += 4
            return ('union', active, tag, self.value(t))
        if typ == T_INTLIST:
            n, self.pos = dec_varint(d, self.pos)
            out = []
            for _ in range(n):
                v, self.pos = dec_varint(d, self.pos)
                out.append(v)
            return out
        if typ == T_PAIR:
            a, self.pos = dec_varint(d, self.pos)
            b, self.pos = dec_varint(d, self.pos)
            return (a, b)
        if typ == T_TRIPLE:
            a, self.pos = dec_varint(d, self.pos)
            b, self.pos = dec_varint(d, self.pos)
            c, self.pos = dec_varint(d, self.pos)
            return (a, b, c)
        if typ == T_FLOAT:
            v = struct.unpack('>f', d[self.pos:self.pos + 4])[0]
            self.pos += 4
            return v
        raise ValueError(f'unknown TDF type {typ} at {self.pos}')

    def members(self, struct_end=False):
        out = []
        d = self.data
        while self.pos < len(d):
            if struct_end and d[self.pos] == 0:
                self.pos += 1
                break
            tag = tag_name(d[self.pos:self.pos + 3])
            typ = d[self.pos + 3]
            self.pos += 4
            out.append((tag, typ, self.value(typ)))
        return out


def decode(body):
    return Reader(body).members()


# Encoding: members are (tag, type, value); nested structs are lists of members.
def enc_value(typ, value):
    if typ == T_INT:
        return enc_varint(int(value))
    if typ == T_STRING:
        raw = value.encode('utf-8') + b'\0'
        return enc_varint(len(raw)) + raw
    if typ == T_BLOB:
        return enc_varint(len(value)) + bytes(value)
    if typ == T_STRUCT:
        return encode(value) + b'\0'
    if typ == T_LIST:
        sub, items = value
        out = bytes([sub]) + enc_varint(len(items))
        for v in items:
            if isinstance(v, UnionItem):
                out += bytes([v[0]]) + (b'' if v[1] is None else encode(v[1]) + b'\0')
            else:
                out += enc_value(sub, v)
        return out
    if typ == T_MAP:
        kt, vt, items = value
        out = bytes([kt, vt]) + enc_varint(len(items))
        for k, v in items:
            out += enc_value(kt, k) + enc_value(vt, v)
        return out
    if typ == T_UNION:
        if value is None:
            return b'\x7f'
        active, (tag, t, v) = value
        return bytes([active]) + tag_bytes(tag) + bytes([t]) + enc_value(t, v)
    if typ == T_INTLIST:
        return enc_varint(len(value)) + b''.join(enc_varint(v) for v in value)
    if typ == T_PAIR:
        return enc_varint(value[0]) + enc_varint(value[1])
    if typ == T_TRIPLE:
        return enc_varint(value[0]) + enc_varint(value[1]) + enc_varint(value[2])
    if typ == T_FLOAT:
        return struct.pack('>f', value)
    raise ValueError(typ)


def encode(members):
    return b''.join(tag_bytes(tag) + bytes([typ]) + enc_value(typ, value) for tag, typ, value in members)


def frame(component, command, body, msg_type=MSG_REPLY, msg_id=0, error=0):
    return struct.pack('>HHHHHH', len(body), component, command, error, msg_type, msg_id) + body


def parse_header(data):
    length, component, command, error, flags, msg_id = struct.unpack('>HHHHHH', data[:12])
    return length, component, command, error, flags, msg_id


def pretty(members, indent=0):
    lines = []
    pad = '  ' * indent
    for tag, typ, value in members:
        if typ == T_STRUCT:
            lines.append(f'{pad}{tag} struct')
            lines.extend(pretty(value, indent + 1))
        elif typ == T_UNION and isinstance(value, tuple) and len(value) == 4:
            _, active, utag, uval = value
            if isinstance(uval, list) and uval and isinstance(uval[0], tuple):
                lines.append(f'{pad}{tag} union[{active}] {utag}')
                lines.extend(pretty(uval, indent + 1))
            else:
                lines.append(f'{pad}{tag} union[{active}] {utag} = {uval!r}')
        elif typ == T_LIST and value and all(isinstance(v, UnionItem) for v in value):
            for i, (active, members_) in enumerate(value):
                lines.append(f'{pad}{tag}[{i}] union[{active}]')
                lines.extend(pretty(members_ or [], indent + 1))
        elif typ == T_LIST and value and all(isinstance(v, list) and v and isinstance(v[0], tuple) for v in value):
            for i, members_ in enumerate(value):
                lines.append(f'{pad}{tag}[{i}] struct')
                lines.extend(pretty(members_, indent + 1))
        else:
            lines.append(f'{pad}{tag} t{typ} = {value!r}')
    return lines


if __name__ == '__main__':
    import sys
    raw = bytes.fromhex(sys.argv[1])
    print(parse_header(raw))
    print('\n'.join(pretty(decode(raw[12:]))))
