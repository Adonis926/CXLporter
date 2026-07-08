import time

# OpenWhisk Python runtime 的同构 workload。
# 当前主测试默认使用 Node.js；该文件用于切换 RUNTIME=python:* 时保持函数形状一致。
PAGE = 4096


def _pattern(i, kind):
    # 为不同函数生成稳定输入，便于和 CXLporter 合成函数做趋势对比。
    if kind == "json":
        s = b'{"key":12345,"value":"abcdef","items":[1,2,3,4]},'
        return s[i % len(s)]
    if kind == "html":
        s = b'<div class="item"><span>cxlfork benchmark</span></div>\n'
        return s[i % len(s)]
    return ((i * 1315423911 + len(kind) * 17) >> 8) & 0xff


def _buf(n, kind):
    # 模拟 action 初始化阶段的输入数据构造。
    return bytearray(_pattern(i, kind) for i in range(n))


def _touch(b):
    # 每页读一次，主要观察内存访问成本。
    acc = 0
    for i in range(0, len(b), PAGE):
        acc ^= b[i]
    return acc


def _json(b):
    score = 0
    depth = 0
    for i, c in enumerate(b):
        if c == ord('{') or c == ord('['):
            depth += 1
        elif (c == ord('}') or c == ord(']')) and depth > 0:
            depth -= 1
        elif ord('0') <= c <= ord('9'):
            score += (c - ord('0')) * (depth + 1)
        elif c == ord('"'):
            score ^= i
    return score ^ depth


def _pyaes(b):
    s0 = 0x00112233
    s1 = 0x44556677
    s2 = 0x8899AABB
    s3 = 0xCCDDEEFF
    for c in b:
        s0 = (((s0 << 5) | (s0 >> 27)) & 0xffffffff) ^ ((c + s3) & 0xffffffff)
        s1 = ((((s1 << 7) | (s1 >> 25)) & 0xffffffff) + (s0 ^ ((c * 0x45D9F3B) & 0xffffffff))) & 0xffffffff
        s2 = ((((s2 << 11) | (s2 >> 21)) & 0xffffffff) ^ ((s1 + 0x9E3779B9) & 0xffffffff)) & 0xffffffff
        s3 = ((((s3 << 13) | (s3 >> 19)) & 0xffffffff) + (s2 ^ c)) & 0xffffffff
    return ((s0 << 32) ^ s1 ^ s2 ^ s3) & 0xffffffffffffffff


def _html(b):
    tags = 0
    text = 0
    in_tag = False
    for c in b:
        if c == ord('<'):
            in_tag = True
            tags += 1
        elif c == ord('>'):
            in_tag = False
        elif not in_tag and c > 32:
            text += c
    return (tags << 32) ^ text


def _float(b):
    acc = 1.000001
    for c in b:
        acc = acc * 1.0000001 + (c + 1) * 0.0009765625
        if acc > 4096.0:
            acc *= 0.25
    return int(acc * 1000000)


def main(args):
    # OpenWhisk 入口：args 来自 action invoke 的 JSON 参数。
    kind = args.get("function", "json")
    n = int(args.get("bytes", 1048576))
    start = time.monotonic_ns()
    b = _buf(n, kind)
    init_ns = time.monotonic_ns() - start
    work_start = time.monotonic_ns()
    if kind == "touch":
        checksum = _touch(b)
    elif kind == "json":
        checksum = _json(b)
    elif kind == "pyaes":
        checksum = _pyaes(b)
    elif kind == "html":
        checksum = _html(b)
    elif kind == "float":
        checksum = _float(b)
    else:
        checksum = _touch(b)
    workload_ns = time.monotonic_ns() - work_start
    return {
        "function": kind,
        "bytes": n,
        "init_ns": init_ns,
        "workload_ns": workload_ns,
        "checksum": checksum,
    }
