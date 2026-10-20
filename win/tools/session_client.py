#!/usr/bin/env python3
"""wetype session_v2 管道 E2E 客户端（Linux 侧）

通过 wine pipebridge 连 wetype_server 的命名管道，
按 commands.proto 完整走一遍 CreateSession → SendKey → 收候选。
"""
import subprocess, sys, time, os
sys.path.insert(0, os.path.dirname(__file__))
from pbpool import load_pool
from google.protobuf import message_factory

PIPE = sys.argv[1] if len(sys.argv) > 1 else \
    '\\\\.\\pipe\\wetype.838180e977848124a1b2f9a7258c4e70.session_v2'
ENGINE_DIR = os.path.expanduser('~/tmp/wetype-win/extracted/3.0.1.1')
WINEPREFIX = os.path.expanduser('~/tmp/wetype-win/prefix')

pool = load_pool()
fd = pool.FindFileByName('commands.proto')
Msg = {n: message_factory.GetMessageClass(m) for n, m in fd.message_types_by_name.items()}

proc = subprocess.Popen(
    ['wine', os.path.join(ENGINE_DIR, 'pipebridge.exe'), PIPE],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
    env={**os.environ, 'WINEPREFIX': WINEPREFIX, 'WINEDEBUG': '-all'},
    cwd=ENGINE_DIR)

def send(msg):
    proc.stdin.write(msg.SerializeToString())
    proc.stdin.flush()

def recv(timeout=5.0):
    """读一条服务器消息（阻塞，带超时）"""
    import select
    r, _, _ = select.select([proc.stdout], [], [], timeout)
    if not r:
        return None
    data = proc.stdout.readline()  # 行缓冲不适用于二进制！
    return data

# 二进制管道不能用 readline —— 用 os.read 非阻塞读
def recv_msg(timeout=5.0):
    import select, fcntl
    fl = fcntl.fcntl(proc.stdout, fcntl.F_GETFL)
    fcntl.fcntl(proc.stdout, fcntl.F_SETFL, fl | os.O_NONBLOCK)
    deadline = time.time() + timeout
    buf = b''
    while time.time() < deadline:
        r, _, _ = select.select([proc.stdout], [], [], 0.2)
        if r:
            chunk = os.read(proc.stdout.fileno(), 65536)
            if chunk:
                buf += chunk
                # 消息模式：bridge 每次 ReadFile=一条消息，但 os.read 可能拆/并
                # wetype 无长度前缀；逐条解析：protobuf 消息在此场景下单条到达
                if len(buf) > 0:
                    fcntl.fcntl(proc.stdout, fcntl.F_SETFL, fl)
                    return buf
    fcntl.fcntl(proc.stdout, fcntl.F_SETFL, fl)
    return buf if buf else None

def parse_resp(data):
    m = Msg['RespBase'].FromString(data)
    body = Msg['RespBodyV1'].FromString(m.resp)
    return m, body

# 1) 服务器主动推的第一条
first = recv_msg(6.0)
if first:
    m, body = parse_resp(first)
    print(f"[push] version={m.version} pid={body.pid} detail_len={len(body.detail)}")
else:
    print("[no initial push — maybe fine]")

# 1.5) 先自报身份：IPCPathInfo（mozc 传统：客户端首条 = 密钥握手）
ipc_fdp = pool.FindFileByName('ipc.proto')
IPCMeta = {n: message_factory.GetMessageClass(m) for n, m in ipc_fdp.message_types_by_name.items()}
ident = IPCMeta['IPCPathInfo']()
ident.key = PIPE.split('.')[-2]          # 管道 key
ident.process_id = os.getpid()
ident.thread_id = 0
ident.protocol_version = 2               # session_v2
ident.product_version = '3.0.1.1'
send(ident)
print("[sent IPCPathInfo]")
r = recv_msg(3.0)
print(f"[ident resp] {r.hex(' ') if r else None}")

# 2) CreateSession
def make_cmd(ctor):
    cv1 = Msg['CommandV1']()
    cv1.application_info.process_name = 'fcitx5-wetype'
    cv1.application_info.client_type = 1  # CLIENT_TIP
    cv1.application_info.process_id = os.getpid()
    ctor(cv1.cmd.add())
    cmd = Msg['CommandBase'](version=1)
    cmd.cmd = cv1.SerializeToString()
    return cmd

cmd = make_cmd(lambda d: setattr(d, 'command_type', 3))  # CREATE_SESSION
send(cmd)
print("[sent CREATE_SESSION]")
r = recv_msg(6.0)
if r:
    m, body = parse_resp(r)
    print(f"[resp] pid={body.pid} detail[:80]={body.detail[:80].hex(' ')}")
else:
    print("[no resp to CREATE_SESSION]")

# 3) SendKey n i h a o
for ch in 'nihao':
    def key(d, ch=ch):
        d.command_type = 1  # SEND_KEY
        d.key.key_code = ord(ch)
        d.key.modifier_keys.append(8)  # KEY_DOWN
    send(make_cmd(key))
    time.sleep(0.25)
    r = recv_msg(3.0)
    if r:
        try:
            m, body = parse_resp(r)
            print(f"[key {ch}] resp detail[:120]={body.detail[:120].hex(' ')}")
        except Exception as e:
            print(f"[key {ch}] parse fail: {e} raw={r[:60].hex(' ')}")
    else:
        print(f"[key {ch}] no resp")

proc.stdin.close()
proc.terminate()
