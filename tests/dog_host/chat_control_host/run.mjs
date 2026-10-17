import { loadWindowsToolchainEnvironment } from '../common/msvc-env.mjs';
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { resolve, dirname } from 'node:path';

const tests = dirname(fileURLToPath(import.meta.url));
const root = resolve(tests, '../../..');
const work = resolve(root, 'build/dog_host/chat_control_host');
const main = resolve(root, 'main');
const source = readFileSync(resolve(main, 'application.cc'), 'utf8').replaceAll('\r\n', '\n');
mkdirSync(work, { recursive: true });

function extractFunction(signature) {
  const start = source.indexOf(signature + ' {');
  const end = source.indexOf('\n}', start);
  if (start < 0 || end < 0) throw new Error('Cannot extract actual function: ' + signature);
  return source.slice(start, end + 2);
}
function extractEnum(path, name) {
  const text = readFileSync(path, 'utf8');
  const match = text.match(new RegExp('enum ' + name + ' \\{[\\s\\S]*?\\};'));
  if (!match) throw new Error('Cannot extract actual enum: ' + name);
  return match[0];
}
writeFileSync(resolve(work, 'actual_enums.inc'), [
  extractEnum(resolve(main, 'application.h'), 'AecMode'),
  extractEnum(resolve(main, 'protocols/protocol.h'), 'AbortReason'),
  extractEnum(resolve(main, 'protocols/protocol.h'), 'ListeningMode'),
].join('\n') + '\n');
writeFileSync(resolve(work, 'actual_application.inc'), [
  'void Application::SetChatActive(bool active)',
  'void Application::Schedule(std::function<void()> callback)',
  'void Application::SetListeningMode(ListeningMode mode)',
  'void Application::AbortSpeaking(AbortReason reason)',
  'void Application::SetDeviceState(DeviceState state)',
].map(extractFunction).join('\n\n') + '\n');

const closeStart = source.indexOf('    protocol_->OnAudioChannelClosed(');
const closeEnd = source.indexOf('    protocol_->OnIncomingJson(', closeStart);
if (closeStart < 0 || closeEnd < 0) throw new Error('Cannot extract actual channel-close registration');
writeFileSync(resolve(work, 'actual_close_callback.inc'),
  'void Application::RegisterCloseCallback() {\n    auto& board = Board::GetInstance();\n' +
  source.slice(closeStart, closeEnd) + '}\n');

const ttsStart = source.indexOf('            if (strcmp(state->valuestring, "start") == 0) {');
const ttsEnd = source.indexOf('            } else if (strcmp(state->valuestring, "stop") == 0) {', ttsStart);
if (ttsStart < 0 || ttsEnd < 0) throw new Error('Cannot extract actual TTS start queue body');
writeFileSync(resolve(work, 'actual_tts_start.inc'),
  'void Application::ReceiveTtsStart() {\n' + source.slice(source.indexOf('\n', ttsStart) + 1, ttsEnd) + '}\n');

const env = loadWindowsToolchainEnvironment(process.env);
const exe = resolve(work, 'chat-control-host.exe');
const build = spawnSync('cl.exe', [
  '/nologo', '/std:c++20', '/EHsc', '/MD', '/W4', '/utf-8',
  '/I' + work, '/I' + main, resolve(tests, 'test.cc'), '/Fe' + exe,
], { cwd: work, env, encoding: 'utf8', windowsHide: true });
const buildOutput = (build.stdout ?? '') + (build.stderr ?? '');
writeFileSync(resolve(work, 'build.log'), buildOutput);
if (build.error) throw build.error;
if (build.status !== 0) {
  process.stdout.write(buildOutput);
  process.exit(build.status ?? 1);
}
const run = spawnSync(exe, [], { cwd: work, env, encoding: 'utf8', windowsHide: true, timeout: 10000 });
const output = (run.stdout ?? '') + (run.stderr ?? '');
writeFileSync(resolve(work, 'test.log'), output);
writeFileSync(resolve(work, 'result.json'), JSON.stringify({
  subject: 'Actual SetChatActive, Schedule, SetListeningMode, AbortSpeaking, SetDeviceState, audio-channel-close callback and TTS-start closure extracted from main/application.cc',
  toolchain: 'existing Windows MSVC; no downloads',
  limits: 'native behavioral test with protocol/audio/display/event stubs and real queued callback bodies; physical audio and real server acceptance are separate',
  status: run.status, error: run.error?.message ?? null, stdout: run.stdout, stderr: run.stderr,
}, null, 2) + '\n');
process.stdout.write(output);
if (run.error) throw run.error;
process.exit(run.status ?? 1);
