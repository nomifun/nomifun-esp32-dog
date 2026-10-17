import { loadWindowsToolchainEnvironment } from '../common/msvc-env.mjs';
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { resolve, dirname } from 'node:path';

const tests = dirname(fileURLToPath(import.meta.url));
const repo = resolve(tests, '../../..');
const work = resolve(repo, 'build/dog_host/network_reset_host');
const board = resolve(repo, 'main/boards/zfsn-esp32s3-dog');
const main = resolve(repo, 'main');
mkdirSync(work, { recursive: true });
const env = loadWindowsToolchainEnvironment(process.env);
const build = spawnSync('cl.exe', [
  '/nologo', '/std:c++20', '/EHsc', '/MD', '/W4',
  '/I' + resolve(tests, 'stubs'), '/I' + board, '/I' + main,
  resolve(tests, 'test.cc'), resolve(board, 'network_reset.cc'), resolve(main, 'settings.cc'),
  '/Fe' + resolve(work, 'network-reset-host.exe'),
], { cwd: work, env, encoding: 'utf8', windowsHide: true });
const buildOutput = (build.stdout ?? '') + (build.stderr ?? '');
writeFileSync(resolve(work, 'build.log'), buildOutput);
process.stdout.write(buildOutput);
if (build.error) throw build.error;
if (build.status !== 0) process.exit(build.status ?? 1);
const result = spawnSync(resolve(work, 'network-reset-host.exe'), [], {
  cwd: work, env, encoding: 'utf8', windowsHide: true, timeout: 10000,
});
const output = (result.stdout ?? '') + (result.stderr ?? '');
writeFileSync(resolve(work, 'test.log'), output);
writeFileSync(resolve(work, 'result.json'), JSON.stringify({
  status: result.status, error: result.error?.message ?? null,
  toolchain: 'existing Windows MSVC; no WSL or downloads',
  subject: 'actual network_reset.cc and settings.cc with commit-aware NVS host stub',
  limits: 'host persistence and failure model; physical reboot/AP acceptance is separate',
  stdout: result.stdout, stderr: result.stderr,
}, null, 2) + '\n');
process.stdout.write(output);
if (result.error) throw result.error;
process.exit(result.status ?? 1);
