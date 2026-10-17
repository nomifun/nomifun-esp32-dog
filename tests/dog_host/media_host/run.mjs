import { loadWindowsToolchainEnvironment } from '../common/msvc-env.mjs';
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { resolve, dirname } from 'node:path';
import { createHash } from 'node:crypto';

const tests = dirname(fileURLToPath(import.meta.url));
const root = resolve(tests, '../../..');
const board = resolve(root, 'main/boards/zfsn-esp32s3-dog');
const work = resolve(root, 'build/dog_host/media_host');
mkdirSync(work, { recursive: true });
// Each run receives a fresh isolated SD fixture and never touches device data.
const sd = resolve(work, 'sd-' + Date.now(), 'music', 'mp3');
const original = readFileSync(resolve(board, 'dog_media.cc'), 'utf8');
const token = '"/sdcard/music/mp3"';
if (original.split(token).length !== 2) throw new Error('Expected one host SD path substitution');
writeFileSync(resolve(work, 'dog_media.cc'), original.replace(token, JSON.stringify(sd.replaceAll('\\', '/'))));
const env = loadWindowsToolchainEnvironment(process.env);
const build = spawnSync('cl.exe', [
  '/nologo', '/std:c++20', '/EHsc', '/MD', '/utf-8', '/W4', '/D_CRT_SECURE_NO_WARNINGS',
  '/I' + resolve(tests, 'stubs'), '/I' + resolve(tests, '../pet_dog_host/win32'), '/I' + board,
  '/FI' + resolve(tests, 'compat.h'),
  resolve(tests, 'test.cc'), resolve(work, 'dog_media.cc'), '/Fe' + resolve(work, 'media-host.exe'),
], { cwd: work, env, encoding: 'utf8', windowsHide: true });
const buildOutput = (build.stdout ?? '') + (build.stderr ?? '');
writeFileSync(resolve(work, 'build.log'), buildOutput);
process.stdout.write(buildOutput);
if (build.error) throw build.error;
if (build.status !== 0) process.exit(build.status ?? 1);
const result = spawnSync(resolve(work, 'media-host.exe'), [sd], {
  cwd: work, env, encoding: 'utf8', windowsHide: true, timeout: 10000,
});
const output = (result.stdout ?? '') + (result.stderr ?? '');
writeFileSync(resolve(work, 'test.log'), output);
const hashes = Object.fromEntries(['dog_media.cc', 'dog_media.h'].map(name => [name,
  createHash('sha256').update(readFileSync(resolve(board, name))).digest('hex')]));
for (const name of ['test.cc', 'stubs/fake.h', 'compat.h'])
  hashes[name] = createHash('sha256').update(readFileSync(resolve(tests, name))).digest('hex');
writeFileSync(resolve(work, 'result.json'), JSON.stringify({
  status: result.status, error: result.error?.message ?? null,
  toolchain: 'existing Windows MSVC; no downloads, network changes, USB or hardware access',
  subject: 'actual dog_media.cc; only SD root is substituted, clock/RTOS/NVS/decoder/audio are host substitutes',
  hashes,
  limits: 'proves media state and recovery behavior; real MP3 decoder, SD/FatFS, I2S sound and ESP32 timing require device acceptance',
  stdout: result.stdout, stderr: result.stderr,
}, null, 2) + '\n');
process.stdout.write(output);
if (result.error) throw result.error;
process.exit(result.status ?? 1);
