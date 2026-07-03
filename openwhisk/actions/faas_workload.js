'use strict';

const PAGE = 4096;

function nowNs() {
  return process.hrtime.bigint();
}

function pattern(i, kind) {
  if (kind === 'json') {
    const s = Buffer.from('{"key":12345,"value":"abcdef","items":[1,2,3,4]},');
    return s[i % s.length];
  }
  if (kind === 'html') {
    const s = Buffer.from('<div class="item"><span>cxlfork benchmark</span></div>\n');
    return s[i % s.length];
  }
  return ((Math.imul(i, 1315423911) + kind.length * 17) >>> 8) & 0xff;
}

function makeBuffer(n, kind) {
  const b = Buffer.allocUnsafe(n);
  for (let i = 0; i < n; i += 1) {
    b[i] = pattern(i, kind);
  }
  return b;
}

function touch(b) {
  let acc = 0;
  for (let i = 0; i < b.length; i += PAGE) {
    acc ^= b[i];
  }
  return acc >>> 0;
}

function jsonWork(b) {
  let score = 0;
  let depth = 0;
  for (let i = 0; i < b.length; i += 1) {
    const c = b[i];
    if (c === 123 || c === 91) {
      depth += 1;
    } else if ((c === 125 || c === 93) && depth > 0) {
      depth -= 1;
    } else if (c >= 48 && c <= 57) {
      score = (score + (c - 48) * (depth + 1)) >>> 0;
    } else if (c === 34) {
      score = (score ^ i) >>> 0;
    }
  }
  return (score ^ depth) >>> 0;
}

function pyaesWork(b) {
  let s0 = 0x00112233;
  let s1 = 0x44556677;
  let s2 = 0x8899aabb;
  let s3 = 0xccddeeff;
  for (const c of b) {
    s0 = (((s0 << 5) | (s0 >>> 27)) ^ ((c + s3) >>> 0)) >>> 0;
    s1 = ((((s1 << 7) | (s1 >>> 25)) + (s0 ^ Math.imul(c, 0x45d9f3b))) >>> 0);
    s2 = ((((s2 << 11) | (s2 >>> 21)) ^ ((s1 + 0x9e3779b9) >>> 0)) >>> 0);
    s3 = ((((s3 << 13) | (s3 >>> 19)) + (s2 ^ c)) >>> 0);
  }
  return (s0 ^ s1 ^ s2 ^ s3) >>> 0;
}

function htmlWork(b) {
  let tags = 0;
  let text = 0;
  let inTag = false;
  for (const c of b) {
    if (c === 60) {
      inTag = true;
      tags += 1;
    } else if (c === 62) {
      inTag = false;
    } else if (!inTag && c > 32) {
      text = (text + c) >>> 0;
    }
  }
  return ((tags & 0xffff) << 16) ^ (text & 0xffff);
}

function floatWork(b) {
  let acc = 1.000001;
  for (const c of b) {
    acc = acc * 1.0000001 + (c + 1) * 0.0009765625;
    if (acc > 4096.0) {
      acc *= 0.25;
    }
  }
  return Math.trunc(acc * 1000000);
}

function main(args) {
  const kind = args.function || 'json';
  const bytes = Number.parseInt(args.bytes || '1048576', 10);
  const initStart = nowNs();
  const b = makeBuffer(bytes, kind);
  const initNs = nowNs() - initStart;
  const workStart = nowNs();
  let checksum;
  if (kind === 'touch') {
    checksum = touch(b);
  } else if (kind === 'json') {
    checksum = jsonWork(b);
  } else if (kind === 'pyaes') {
    checksum = pyaesWork(b);
  } else if (kind === 'html') {
    checksum = htmlWork(b);
  } else if (kind === 'float') {
    checksum = floatWork(b);
  } else {
    checksum = touch(b);
  }
  const workloadNs = nowNs() - workStart;
  return {
    function: kind,
    bytes,
    init_ns: Number(initNs),
    workload_ns: Number(workloadNs),
    checksum,
  };
}

exports.main = main;

if (require.main === module) {
  const args = {};
  for (let i = 2; i < process.argv.length; i += 1) {
    const [key, value] = process.argv[i].split('=', 2);
    args[key] = value;
  }
  Promise.resolve(main(args)).then((result) => {
    process.stdout.write(`${JSON.stringify(result)}\n`);
  }).catch((err) => {
    console.error(err.stack || err);
    process.exit(1);
  });
}
