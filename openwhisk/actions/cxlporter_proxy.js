'use strict';

// The OpenWhisk action is deliberately a small control-plane adapter.  The
// checkpointed function state and workload execute in the VM1 ghost worker.
const net = require('net');
const crypto = require('crypto');

function nowNs() {
  return process.hrtime.bigint();
}

function token(value, fallback) {
  const text = String(value === undefined ? fallback : value);
  if (!/^[A-Za-z0-9_.:-]+$/.test(text)) {
    throw new Error(`invalid protocol token: ${text}`);
  }
  return text;
}

function parseResult(line) {
  const fields = {};
  const parts = line.trim().split(/ +/);
  if (parts[0] !== 'CXLP/1' || parts[1] !== 'RESULT') {
    throw new Error(`bad CXLporter response: ${line.slice(0, 160)}`);
  }
  for (const part of parts.slice(2)) {
    const at = part.indexOf('=');
    if (at <= 0) {
      throw new Error(`bad CXLporter field: ${part}`);
    }
    fields[part.slice(0, at)] = part.slice(at + 1);
  }
  return fields;
}

function exchange(host, port, line, timeoutMs) {
  return new Promise((resolve, reject) => {
    const begin = nowNs();
    let connected = 0n;
    let firstByte = 0n;
    let data = '';
    let finished = false;
    const socket = net.createConnection({ host, port });

    function fail(err) {
      if (finished) return;
      finished = true;
      socket.destroy();
      reject(err);
    }

    socket.setTimeout(timeoutMs, () => fail(new Error('CXLporter timeout')));
    socket.on('connect', () => {
      connected = nowNs();
      socket.write(`${line}\n`);
    });
    socket.on('data', (chunk) => {
      if (firstByte === 0n) firstByte = nowNs();
      data += chunk.toString('utf8');
      const newline = data.indexOf('\n');
      if (newline < 0) return;
      if (finished) return;
      finished = true;
      const end = nowNs();
      socket.end();
      resolve({
        fields: parseResult(data.slice(0, newline)),
        connect_ns: String(connected - begin),
        first_byte_ns: String(firstByte - begin),
        bridge_rtt_ns: String(end - begin),
      });
    });
    socket.on('error', fail);
    socket.on('end', () => {
      if (!finished) fail(new Error('CXLporter closed before a complete response'));
    });
  });
}

async function main(args) {
  const actionBegin = nowNs();
  const host = token(args.host, '172.17.0.1');
  const port = Number.parseInt(args.port || '2301', 10);
  const operation = token(args.operation, 'INVOKE').toUpperCase();
  const requestId = token(args.request_id,
    `${Date.now().toString(16)}-${crypto.randomBytes(6).toString('hex')}`);
  const timeoutMs = Number.parseInt(args.timeout_ms || '300000', 10);
  const fields = [`CXLP/1 ${operation}`, `id=${requestId}`];

  if (!Number.isInteger(port) || port < 1 || port > 65535) {
    throw new Error(`invalid port: ${args.port}`);
  }
  if (operation === 'INVOKE') {
    fields.push(`function=${token(args.function, 'touch')}`);
    fields.push(`policy=${token(args.policy, 'mow')}`);
    fields.push(`mode=${token(args.mode, 'invoke')}`);
    fields.push(`validation=${token(args.validation, 'fast')}`);
  }

  try {
    const response = await exchange(host, port, fields.join(' '), timeoutMs);
    return {
      request_id: requestId,
      transport: 'tcp-qemu-hostfwd',
      action_total_ns: String(nowNs() - actionBegin),
      action_connect_ns: response.connect_ns,
      action_first_byte_ns: response.first_byte_ns,
      action_bridge_rtt_ns: response.bridge_rtt_ns,
      ...response.fields,
    };
  } catch (err) {
    return {
      request_id: requestId,
      status: String(err.code || 'EIO'),
      verify: '0',
      error_stage: 'action_bridge',
      error: err.message,
      action_total_ns: String(nowNs() - actionBegin),
    };
  }
}

exports.main = main;
