import { loadWindowsToolchainEnvironment } from '../common/msvc-env.mjs';
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { resolve, dirname } from 'node:path';

const tests = dirname(fileURLToPath(import.meta.url));
const root = resolve(tests, '../../..');
const work = resolve(root, 'build/dog_host/ota_setup_host');
const source = readFileSync(resolve(root, 'main/application.cc'), 'utf8');
mkdirSync(work, { recursive: true });
const checkStart = source.indexOf('bool Application::CheckNewVersion(Ota& ota) {');
const checkEnd = source.indexOf('\nvoid Application::ShowActivationCode(', checkStart);
if (checkStart < 0 || checkEnd < 0) throw new Error('Cannot extract actual CheckNewVersion implementation');
writeFileSync(resolve(work, 'actual_check_new_version.inc'), source.slice(checkStart, checkEnd));
const start = source.indexOf('void Application::Start() {');
const startOta = source.indexOf('    Ota ota;', start);
const startProtocol = source.indexOf('    // Initialize the protocol', startOta);
if (start < 0 || startOta < 0 || startProtocol < 0) throw new Error('Cannot extract actual startup OTA gate');
writeFileSync(resolve(work, 'actual_startup_gate.inc'),
  'void Application::Start() {\n' + source.slice(startOta, startProtocol) +
  '    protocol_initialized = true;\n}\n');
const env = loadWindowsToolchainEnvironment(process.env);
const profiles = [
  { name: 'production-dog', flags: ['/DCONFIG_BOARD_TYPE_ZFSN_ESP32S3_DOG=1'] },
  { name: 'other-board', flags: [] },
];
const results = [];
for (const profile of profiles) {
  const exe = resolve(work, profile.name + '.exe');
  const build = spawnSync('cl.exe', [
    '/nologo', '/std:c++20', '/EHsc', '/MD', '/W4', '/utf-8', ...profile.flags,
    '/I' + work, resolve(tests, 'test.cc'), '/Fe' + exe,
  ], { cwd: work, env, encoding: 'utf8', windowsHide: true });
  const buildOutput = (build.stdout ?? '') + (build.stderr ?? '');
  writeFileSync(resolve(work, profile.name + '-build.log'), buildOutput);
  if (build.error) throw build.error;
  if (build.status !== 0) {
    process.stdout.write(buildOutput);
    process.exit(build.status ?? 1);
  }
  const run = spawnSync(exe, [], { cwd: work, env, encoding: 'utf8', windowsHide: true, timeout: 10000 });
  writeFileSync(resolve(work, profile.name + '-test.log'), (run.stdout ?? '') + (run.stderr ?? ''));
  process.stdout.write(profile.name + ': ' + (run.stdout ?? '') + (run.stderr ?? ''));
  results.push({ profile: profile.name, status: run.status, stdout: run.stdout, stderr: run.stderr });
  if (run.error) throw run.error;
  if (run.status !== 0) process.exit(run.status ?? 1);
}
writeFileSync(resolve(work, 'result.json'), JSON.stringify({
  subject: 'Actual CheckNewVersion function and startup OTA gate extracted from main/application.cc',
  toolchain: 'existing Windows MSVC; no WSL or downloads',
  limits: 'native behavioral test with mocked OTA responses and task delay; device and live network acceptance are separate',
  results,
}, null, 2) + '\n');
