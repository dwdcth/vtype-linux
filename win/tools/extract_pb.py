#!/usr/bin/env python3
"""从 PE 提取内嵌 protobuf FileDescriptorProto。

FileDescriptorProto 的全部合法字段都是 wire type 2 (LEN)，
tag ∈ {0x0a,0x12,0x1a,0x22,0x2a,0x32,0x3a,0x42,0x4a,0x52,0x5a,0x62}。
从 0x0A+varint(len)+<name>.proto 开头，按字段边界扫描到首个非法 tag 止。
"""
import struct, sys, os
from google.protobuf import descriptor_pb2

LEN_TAGS = {0x0a,0x12,0x1a,0x22,0x2a,0x32,0x3a,0x42,0x4a,0x52,0x5a,0x62}

def read_varint(b, i):
    v, s = 0, 0
    while i < len(b) and s < 10:
        v |= (b[i] & 0x7f) << s
        s += 7
        if not (b[i] & 0x80):
            return v, i + 1
        i += 1
    raise ValueError

def sections(data):
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    opt = pe + 24
    nsec = struct.unpack_from('<H', data, pe + 6)[0]
    so = opt + struct.unpack_from('<H', data, pe + 20)[0]
    for i in range(nsec):
        o = so + i * 40
        name = data[o:o+8].rstrip(b'\0').decode('latin1')
        vsz, va, rsz, ro = struct.unpack_from('<IIII', data, o+8)
        yield name, ro, rsz

def scan_blob(blob, i):
    """返回 blob 内从 i 开始的完整 descriptor 字节数，非法则 0"""
    n = len(blob)
    while i < n:
        if blob[i] not in LEN_TAGS:
            break
        try:
            ln, j = read_varint(blob, i + 1)
        except ValueError:
            break
        if ln > n - j:
            break
        i = j + ln
    return i

def main():
    pe_path, sub = sys.argv[1], sys.argv[2]
    outdir = sys.argv[3] if len(sys.argv) > 3 else 'protos'
    os.makedirs(outdir, exist_ok=True)
    data = open(pe_path, 'rb').read()
    results = {}
    for sname, ro, rsz in sections(data):
        if sname not in ('.rdata', '.data'):
            continue
        blob = data[ro:ro+rsz]
        i = 0
        while True:
            i = blob.find(b'\x0a', i)
            if i < 0 or i > len(blob) - 8:
                break
            try:
                vlen, j = read_varint(blob, i + 1)
            except ValueError:
                i += 1; continue
            if not (5 <= vlen <= 100) or j + vlen > len(blob):
                i += 1; continue
            fname = blob[j:j+vlen]
            if not fname.endswith(b'.proto') or b'/' in fname:
                i += 1; continue
            end = scan_blob(blob, j + vlen)
            if end <= j + vlen:
                i += 1; continue
            fdp = descriptor_pb2.FileDescriptorProto()
            try:
                fdp.ParseFromString(blob[i:end])
            except Exception:
                i += 1; continue
            if fdp.name == fname.decode():
                results[fdp.name] = (fdp, sname, i)
                i = end
                continue
            i += 1
    hits = {k: v for k, v in results.items() if sub.lower() in k.lower()}
    print(f"共 {len(results)} 个 descriptor，匹配 '{sub}' 的 {len(hits)} 个：")
    for name, (fdp, sec, off) in sorted(hits.items()):
        safe = name.replace('/', '_')
        with open(os.path.join(outdir, safe + '.desc.bin'), 'wb') as f:
            f.write(fdp.SerializeToString())
        print(f"  {name} @{sec}+0x{off:x} deps={list(fdp.dependency)} msgs={[m.name for m in fdp.message_type][:8]}{'...' if len(fdp.message_type)>8 else ''}")

if __name__ == '__main__':
    main()
