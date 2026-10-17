import { loadWindowsToolchainEnvironment } from '../common/msvc-env.mjs';
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync, writeFileSync, rmSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { resolve, dirname } from 'node:path';
import { createHash } from 'node:crypto';

const tests = dirname(fileURLToPath(import.meta.url));
const root = resolve(tests, '../../..');
const board = resolve(root, 'main/boards/zfsn-esp32s3-dog');
const work = resolve(root, 'build/dog_host/pet_dog_host');
const sd = resolve(work, 'sdcard');
mkdirSync(work, { recursive: true });
// This runner owns only this fixed host-test SD directory; never device storage.
if (!sd.startsWith(work + '\\')) throw new Error('Host SD path escaped test work directory');
rmSync(sd, { recursive: true, force: true });
mkdirSync(sd, { recursive: true });
if (!process.env.IDF_PATH) throw new Error('IDF_PATH must point to the installed ESP-IDF (for components/json/cJSON)');
const cjson = resolve(process.env.IDF_PATH, 'components/json/cJSON');
const original = readFileSync(resolve(board, 'pet_dog.cc'), 'utf8');
const token = '"/sdcard/custom_action"';
if (original.split(token).length !== 2) throw new Error('Expected exactly one test SD path substitution');
writeFileSync(resolve(work, 'pet_dog.cc'), original.replace(token, JSON.stringify(resolve(sd, 'custom_action').replaceAll('\\', '/'))));
const env = loadWindowsToolchainEnvironment(process.env);
const build = spawnSync('cl.exe', [
  '/nologo', '/TP', '/std:c++20', '/EHsc', '/MD', '/utf-8', '/W3', '/D_CRT_SECURE_NO_WARNINGS',
  '/I' + resolve(tests, 'win32'), '/I' + resolve(tests, 'stubs'), '/I' + board, '/I' + cjson,
  '/FI' + resolve(tests, 'win32/compat.h'),
  resolve(tests, 'test.cc'), resolve(work, 'pet_dog.cc'), resolve(cjson, 'cJSON.c'),
  '/Fe' + resolve(work, 'pet-dog-host.exe'),
], { cwd: work, env, encoding: 'utf8', windowsHide: true });
const buildOutput = (build.stdout ?? '') + (build.stderr ?? '');
writeFileSync(resolve(work, 'build.log'), buildOutput);
process.stdout.write(buildOutput);
if (build.error) throw build.error;
if (build.status !== 0) process.exit(build.status ?? 1);
const result = spawnSync(resolve(work, 'pet-dog-host.exe'), [], {
  cwd: work, env, encoding: 'utf8', windowsHide: true, timeout: 60000,
});
const output = (result.stdout ?? '') + (result.stderr ?? '');
writeFileSync(resolve(work, 'test.log'), output);
const hashes = Object.fromEntries(['pet_dog.cc', 'pet_dog.h'].map(name => [name,
  createHash('sha256').update(readFileSync(resolve(board, name))).digest('hex')]));
for (const name of ['test.cc', 'stubs/fake.h', 'win32/compat.h', 'win32/dirent.h'])
  hashes[name] = createHash('sha256').update(readFileSync(resolve(tests, name))).digest('hex');
writeFileSync(resolve(work, 'result.json'), JSON.stringify({
  status: result.status, error: result.error?.message ?? null,
  toolchain: 'existing Windows MSVC; no WSL, downloads, network changes, USB or hardware access',
  subject: 'actual PetDog C++ with test SD path substitution and Windows-only POSIX file adapters',
  hashes,
  limits: 'host FreeRTOS/LEDC/NVS substitutes; does not prove server MCP invocation, ESP32 timing or physical movement',
  stdout: result.stdout, stderr: result.stderr,
}, null, 2) + '\n');
process.stdout.write(output);
if (result.error) throw result.error;
process.exit(result.status ?? 1);
