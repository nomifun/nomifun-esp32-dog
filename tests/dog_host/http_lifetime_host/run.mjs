import { loadWindowsToolchainEnvironment } from '../common/msvc-env.mjs';
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { resolve, dirname } from 'node:path';
const tests=dirname(fileURLToPath(import.meta.url));
const root=resolve(tests,'../../..');
const work=resolve(root,'build/dog_host/http_lifetime_host');
const component=process.env.HTTP_LIFETIME_COMPONENT ?? resolve(root,'components/78__esp-ml307');
mkdirSync(work,{recursive:true});
const env=loadWindowsToolchainEnvironment(process.env);
const baseline=process.argv.includes('--baseline');
const name=baseline?'baseline':'fixed';
const build=spawnSync('cl.exe',['/nologo','/std:c++20','/EHsc','/utf-8','/MD','/W3','/D_CRT_SECURE_NO_WARNINGS','/I'+resolve(tests,'stubs'),'/I'+resolve(component,'include'),'/I'+resolve(component,'src/esp'),resolve(tests,'test.cc'),...['http_client.cc','web_socket.cc','esp/esp_tcp.cc','esp/esp_ssl.cc'].map(x=>resolve(component,'src',x)),'/Fe'+resolve(work,name+'.exe')],{cwd:work,env,encoding:'utf8',windowsHide:true});
writeFileSync(resolve(work,name+'-build.log'),(build.stdout??'')+(build.stderr??''));
if(build.status!==0){process.stdout.write((build.stdout??'')+(build.stderr??''));process.exit(build.status??1);}
const run=spawnSync(resolve(work,name+'.exe'),baseline?['--baseline']:[],{cwd:work,env,encoding:'utf8',windowsHide:true,timeout:30000});
const result={component,baseline,status:run.status,error:run.error?.message??null,stdout:run.stdout,stderr:run.stderr,limits:'Actual HTTP/WebSocket/TCP/TLS sources on Windows MSVC with deterministic FreeRTOS/socket/TLS shims. No real network or device. Native transport callbacks/concurrency, not ESP scheduler proof.'};
writeFileSync(resolve(work,name+'-result.json'),JSON.stringify(result,null,2)+'\n');
process.stdout.write((run.stdout??'')+(run.stderr??''));
if(run.error)throw run.error;
process.exit(run.status??1);

