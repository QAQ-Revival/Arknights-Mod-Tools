const fs = require('node:fs'), path = require('node:path'), crypto = require('node:crypto');
const { spawnSync } = require('node:child_process');
const root = path.resolve(__dirname, '..'), build = path.join(root, 'build'), dist = path.join(root, 'dist');
const python = process.env.QAQR_PYTHON || 'python';
function run(exe, args, options = {}) {
  const result = spawnSync(exe, args, { cwd: root, windowsHide: true, encoding: 'utf8', maxBuffer: 8 * 1024 * 1024, ...options });
  if (result.status !== 0) throw Error(result.error?.message || result.stderr || result.stdout || 'Build command failed');
  return result.stdout.trim();
}
function fresh(directory) {
  const resolved = path.resolve(directory);
  if (!resolved.startsWith(build + path.sep) && !resolved.startsWith(dist + path.sep)) throw Error('Unsafe build directory');
  if (fs.existsSync(resolved) && fs.lstatSync(resolved).isSymbolicLink()) throw Error('Linked build directory');
  fs.rmSync(resolved, { recursive: true, force: true }); fs.mkdirSync(resolved, { recursive: true });
}
function hash(file) { return crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex'); }
fs.mkdirSync(build, { recursive: true }); fs.mkdirSync(dist, { recursive: true });
const bot = ['-c', 'user.name=qaq-revival-publisher[bot]', '-c', 'user.email=338412940+qaq-revival-publisher[bot]@users.noreply.github.com', '-c', 'credential.helper='];
const dependencies = {};
for (const name of ['minhook', 'UnityPy', 'Ark-Unpacker']) {
  const directory = path.join(root, 'vendor', name);
  if (!fs.existsSync(directory)) throw Error('Initialize recursive submodules before building');
  dependencies[name] = run('git', [...bot, '-C', directory, 'rev-parse', 'HEAD']);
}
const pins = JSON.parse(fs.readFileSync(path.join(root, 'sources.json'), 'utf8'));
let sourceCommit = null;
try { sourceCommit = run('git', [...bot, 'rev-parse', 'HEAD']); } catch { /* An initial checkout can be built before its first commit. */ }
const sourceDirty = !!run('git', [...bot, 'status', '--porcelain', '--ignore-submodules=all']);
const vswhere = path.join(process.env['ProgramFiles(x86)'] || 'C:/Program Files (x86)', 'Microsoft Visual Studio/Installer/vswhere.exe');
const visualStudio = run(vswhere, ['-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath']);
if (!visualStudio) throw Error('Visual C++ x64 build tools are required');
const setup = path.join(visualStudio, 'VC/Auxiliary/Build/vcvars64.bat');
const output = run('cmd.exe', ['/d', '/s', '/c', `""${setup}" >nul && set"`], { windowsVerbatimArguments: true });
const compilerEnv = { ...process.env };
for (const line of output.split(/\r?\n/)) { const offset = line.indexOf('='); if (offset > 0) compilerEnv[line.slice(0, offset)] = line.slice(offset + 1); }
const cl = (compilerEnv.Path || compilerEnv.PATH).split(';').map(dir => path.join(dir, 'cl.exe')).find(file => fs.existsSync(file));
if (!cl) throw Error('MSVC compiler not found');
const objects = path.join(build, 'native'); fresh(objects); fresh(path.join(dist, 'loader'));
function compile(sources, extra) {
  const split = extra.indexOf('/link');
  return run(cl, ['/nologo', '/O2', '/MT', '/utf-8', '/DUNICODE', '/D_UNICODE', '/DWIN32_LEAN_AND_MEAN', '/DNOMINMAX', '/Fonative\\', ...(split < 0 ? extra : extra.slice(0, split)), ...sources, ...(split < 0 ? [] : extra.slice(split))], { cwd: build, env: compilerEnv });
}
// Relative source paths and /pathmap keep local build paths out of our binaries.
const includes = ['/I../vendor/minhook/include', '/I../vendor/minhook/src', '/I../native', `/pathmap:${root}=/src`];
const hookSources = ['buffer.c', 'hook.c', 'trampoline.c', 'hde/hde64.c'].map(file => '../vendor/minhook/src/' + file);
compile(hookSources, [...includes, '/c']);
const hookObjects = fs.readdirSync(objects).filter(file => file.endsWith('.obj')).map(file => path.join('native', file));
const common = ['/std:c++20', '/EHsc', ...includes];
compile(['../native/manifest.cpp', '../native/runtime.cpp'], [...common, '/LD', '/Fe../dist/loader/ArkRuntime.dll', ...hookObjects, '/link', '/Brepro']);
compile(['../native/manifest.cpp', '../native/launcher.cpp'], [...common, '/Fe../dist/loader/ArkLoader.exe', '/link', '/Brepro']);
if (process.argv.includes('--test')) {
  compile(['../native/manifest.cpp', '../tests/native_tests.cpp'], [...common, '/Fenative/NativeTests.exe', '/link', '/Brepro']);
  console.log(run(path.join(objects, 'NativeTests.exe'), [path.join(build, 'manifest-test-' + Date.now())]));
  compile(['../native/manifest.cpp', '../tests/runtime_tests.cpp'], [...common, '/Fenative/RuntimeTests.exe', ...hookObjects, '/link', '/Brepro']);
  console.log(run(path.join(objects, 'RuntimeTests.exe'), [path.join(build, 'runtime-test-' + Date.now())], { timeout: 60000 }));
}
const packages = process.env.QAQR_PY_PACKAGES || path.join(root, '.cache/packages');
if (!fs.existsSync(path.join(packages, 'PyInstaller'))) console.log(run(python, ['-m', 'pip', 'install', '--target', packages, '-r', 'python/requirements.txt']));
const tpk = path.join(root, 'vendor/UnityPy/UnityPy/resources/lzma.tpk');
if (fs.readFileSync(tpk).subarray(0, 42).toString().startsWith('version https://git-lfs.github.com/spec/')) run('git', [...bot, '-C', path.join(root, 'vendor/UnityPy'), 'lfs', 'pull']);
const staged = path.join(build, 'python'); fresh(staged);
fs.cpSync(path.join(root, 'vendor/UnityPy/UnityPy'), path.join(staged, 'UnityPy'), { recursive: true });
if (fs.readFileSync(path.join(staged, 'UnityPy/resources/lzma.tpk')).subarray(0, 42).toString().startsWith('version https://git-lfs.github.com/spec/')) throw Error('Fetch UnityPy Git LFS resources before building');
const exportFile = path.join(staged, 'UnityPy/export/__init__.py');
const original = fs.readFileSync(exportFile, 'utf8');
if ((original.match(/AudioClipConverter/g) || []).length !== 2) throw Error('Review the audio exclusion patch for this UnityPy revision');
fs.writeFileSync(exportFile, original.split('\n').filter(line => !line.includes('AudioClipConverter')).join('\n'));
const workerEnv = { ...process.env, PYTHONPATH: [staged, packages].join(path.delimiter), PYINSTALLER_CONFIG_DIR: path.join(root, '.cache/pyinstaller') };
const excluded = ['fmod_toolkit', 'pyfmodex', 'UnityPy.export.AudioClipConverter', 'tkinter', 'numpy', 'pandas', 'openpyxl', 'lxml', 'scipy', 'matplotlib'];
console.log(run(python, ['-m', 'PyInstaller', '--noconfirm', '--clean', '--onedir', '--name', 'ArknightsTools', '--distpath', dist, '--workpath', path.join(build, 'python-freeze'), '--specpath', build, '--paths', staged, '--paths', packages, '--collect-all', 'UnityPy', '--collect-all', 'texture2ddecoder', '--collect-data', 'archspec', ...excluded.flatMap(name => ['--exclude-module', name]), 'python/worker.py'], { env: workerEnv }));
const licenses = path.join(dist, 'licenses'); fs.mkdirSync(licenses, { recursive: true });
fs.copyFileSync(path.join(root, 'LICENSE'), path.join(licenses, 'Arknights-Mod-Tools-LICENSE.txt'));
fs.copyFileSync(path.join(root, 'vendor/minhook/LICENSE.txt'), path.join(licenses, 'MinHook-LICENSE.txt'));
fs.copyFileSync(path.join(root, 'vendor/UnityPy/LICENSE'), path.join(licenses, 'UnityPy-LICENSE.txt'));
fs.cpSync(path.join(root, 'licenses'), licenses, { recursive: true });
for (const dir of fs.readdirSync(packages).filter(name => name.endsWith('.dist-info') && !/fmod|pyfmod|unitypy/i.test(name))) {
  for (const name of fs.readdirSync(path.join(packages, dir)).filter(name => /licen[cs]e|copying/i.test(name))) fs.cpSync(path.join(packages, dir, name), path.join(licenses, dir, name), { recursive: true });
}
const inventory = {};
for (const entry of fs.readdirSync(dist, { recursive: true, withFileTypes: true })) if (entry.isFile()) {
  const file = path.join(entry.parentPath, entry.name), relative = path.relative(dist, file).split(path.sep).join('/');
  if (relative === 'component.json') continue;
  if (/fmod|M4ALauncher|Bootstrapper\.dll/i.test(relative)) throw Error('Opaque or unused audio binary in output: ' + relative);
  inventory[relative] = hash(file);
}
fs.writeFileSync(path.join(dist, 'component.json'), JSON.stringify({ schemaVersion: 1, protocol: 1, source: pins.repository, sourceCommit, sourceDirty, dependencies, buildStatus: 'compiled', gameRuntimeValidated: false, capabilities: ['current-pc-bundle-rebuild', 'assetbundle-file-redirection', 'single-mod-reload-v2'], files: inventory }, null, 2));
console.log('Built source-controlled loader and offline worker; no game executed.');
