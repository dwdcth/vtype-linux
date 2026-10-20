#!/usr/bin/env python3
"""把提取的全部 desc.bin 按依赖序装入 pool，供解码"""
import glob, sys
from google.protobuf import descriptor_pb2, descriptor_pool

def load_pool(desc_dir='../protos/desc'):
    pool = descriptor_pool.DescriptorPool()
    pending = {}
    for f in glob.glob(f'{desc_dir}/*.desc.bin'):
        fdp = descriptor_pb2.FileDescriptorProto.FromString(open(f, 'rb').read())
        pending[fdp.name] = fdp
    while pending:
        progress = False
        for name in list(pending):
            fdp = pending[name]
            if all(dep not in pending for dep in fdp.dependency):
                try:
                    pool.Add(fdp)
                except Exception:
                    pass
                del pending[name]
                progress = True
        if not progress:
            for name in list(pending):  # 兜底强灌
                try: pool.Add(pending[name])
                except Exception: pass
                del pending[name]
    return pool

if __name__ == '__main__':
    import glob, os
    from google.protobuf import descriptor_pb2
    pool = load_pool()
    n = len(glob.glob(os.path.join(os.path.dirname(__file__), '..', 'protos', 'desc', '*.desc.bin')))
    print(f'pool ok: {n} 个 desc 已尝试装载')
