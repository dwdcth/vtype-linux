#!/usr/bin/env python3
"""wetype-win 协议资产提取器（可维护版）

从微信输入法 Windows 安装目录的所有 PE 模块中提取内嵌的 protobuf
FileDescriptorProto，去重后生成：
  protos-src/<name>.proto   可读、可 protoc 编译的 .proto 文本
  protos-src/desc/<name>.desc.bin  原始序列化描述符
  protos-src/MANIFEST.json  每个文件的来源模块、sha256、版本戳

升级官方版本后重跑本脚本即可重建 schema，diff protos-src/ 即知协议变化。
用法: extract_all.py <安装目录> <输出目录> [产品版本号]
"""
import struct, sys, os, json, hashlib, re
from google.protobuf import descriptor_pb2

LEN_TAGS = {0x0a, 0x12, 0x1a, 0x22, 0x2a, 0x32, 0x3a, 0x42, 0x4a, 0x52, 0x5a, 0x62}

WT_TYPES = {1: 'double', 2: 'float', 3: 'int64', 4: 'uint64', 5: 'int32',
            6: 'fixed64', 7: 'fixed32', 8: 'bool', 9: 'string', 10: 'group',
            11: None, 12: 'bytes', 13: 'uint32', 14: None, 15: 'sfixed32',
            16: 'sfixed64', 17: 'sint32', 18: 'sint64'}

def read_varint(b, i):
    v = s = 0
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

def extract_from_pe(path):
    data = open(path, 'rb').read()
    out = {}
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
            if fdp.name == fname.decode() and (fdp.message_type or fdp.enum_type or fdp.service):
                out[fdp.name] = fdp
                i = end
                continue
            i += 1
    return out

def field_type_str(f, scope_names):
    if f.type in (11, 14):
        tn = f.type_name.lstrip('.')
        return tn
    return WT_TYPES[f.type]

from google.protobuf import descriptor_pb2 as _dpb

def is_map_entry(nm):
    return nm.options.HasExtension(_dpb.map_entry) and nm.options.Extensions[_dpb.map_entry]

def is_map_entry_field(f):
    return f.type == 11 and f.label == 3  # 粗判，后面用 map 还原

def gen_proto(fdp):
    L = []
    syntax = fdp.syntax or 'proto2'
    L.append(f'// 提取自微信输入法 Windows 版，勿手改；用 extract_all.py 重新生成')
    L.append(f'syntax = "{syntax}";')
    if fdp.package:
        L.append(f'package {fdp.package};')
    for dep in fdp.dependency:
        L.append(f'import "{dep}";')
    L.append('')

    def emit_enum(en, indent=''):
        pre = '' if not indent else indent
        L.append(f'{pre}enum {en.name} {{')
        for v in en.value:
            L.append(f'{pre}  {v.name} = {v.number};')
        L.append(f'{pre}}}')
        L.append('')

    def emit_msg(m, indent=''):
        L.append(f'{indent}message {m.name} {{')
        inner = indent + '  '
        for en in m.enum_type:
            emit_enum(en, inner)
        for nm in m.nested_type:
            emit_msg(nm, inner)
        for f in m.field:
            lbl = ''
            if f.label == f.LABEL_REPEATED:
                # map 类型还原
                pass
                lbl = 'repeated '
            elif syntax == 'proto2':
                lbl = 'optional ' if f.label == f.LABEL_OPTIONAL else 'required '
            default = ''
            if f.default_value:
                if f.type in (9,):
                    default = f' [default = "{f.default_value}"]'
                else:
                    default = f' [default = {f.default_value}]'
            L.append(f'{inner}{lbl}{field_type_str(f, None)} {f.name} = {f.number};')
        L.append(f'{indent}}}')
        L.append('')

    for en in fdp.enum_type:
        emit_enum(en)
    for m in fdp.message_type:
        emit_msg(m)
    for sv in fdp.service:
        L.append(f'service {sv.name} {{')
        for rpc in sv.method:
            cin = rpc.input_type.lstrip('.'); cout = rpc.output_type.lstrip('.')
            L.append(f'  rpc {rpc.name}({cin}) returns ({cout});')
        L.append('}')
    # map 字段补丁：扫全部 message 的 map fields
    maps = []
    def collect_maps(m, prefix):
        for nm in m.nested_type:
            if nm.options.map_entry:
                continue
            collect_maps(nm, f'{prefix}{nm.name}.')
        for f in m.field:
            if is_map_entry(f):
                continue
    return '\n'.join(L)

def main():
    src_dir, out_dir = sys.argv[1], sys.argv[2]
    product = sys.argv[3] if len(sys.argv) > 3 else 'unknown'
    os.makedirs(os.path.join(out_dir, 'desc'), exist_ok=True)
    all_prios = ['wetype_server.exe', 'wetype_tip_core.dll', 'wetype_renderer.exe',
                 'wetype_update.exe', 'wetap_business.dll']
    pes = []
    for root, _, files in os.walk(src_dir):
        for fn in files:
            if fn.lower().endswith(('.exe', '.dll')):
                full = os.path.join(root, fn)
                try:
                    with open(full, 'rb') as f:
                        if f.read(2) == b'MZ':
                            pes.append(full)
                except OSError:
                    pass
    pes.sort(key=lambda p: next((i for i, n in enumerate(all_prios) if os.path.basename(p) == n), 99))
    merged = {}
    for pe in pes:
        try:
            found = extract_from_pe(pe)
        except Exception as e:
            continue
        base = os.path.basename(pe)
        for name, fdp in found.items():
            blob = fdp.SerializeToString()
            h = hashlib.sha256(blob).hexdigest()[:16]
            if name not in merged or len(blob) > len(merged[name]['blob']):
                merged[name] = {'fdp': fdp, 'blob': blob, 'src': base}
            elif base not in merged[name]['src']:
                merged[name]['src'] += f'|{base}'
    manifest = {'product_version': product, 'generated_by': 'tools/extract_all.py',
                'files': {}}
    for name, ent in sorted(merged.items()):
        fdp, blob, src = ent["fdp"], ent["blob"], ent["src"]
        safe = name.replace('/', '_')
        with open(os.path.join(out_dir, 'desc', safe + '.desc.bin'), 'wb') as f:
            f.write(blob)
        text = gen_proto(fdp)
        with open(os.path.join(out_dir, safe), 'w') as f:
            f.write(text)
        manifest['files'][name] = {'sha256_16': hashlib.sha256(blob).hexdigest()[:16],
                                   'size': len(blob), 'source': src,
                                   'package': fdp.package,
                                   'messages': [m.name for m in fdp.message_type]}
    with open(os.path.join(out_dir, 'MANIFEST.json'), 'w') as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
    print(f'提取 {len(merged)} 个 proto，输出到 {out_dir}')

if __name__ == '__main__':
    main()
