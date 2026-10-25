const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { spawnSync } = require('node:child_process');

function build({ output, test = false } = {}) {
  const componentRoot = path.resolve(__dirname, '..');
  const destination = path.resolve(output || path.join(componentRoot, 'dist/cursor'));
  if (destination !== path.join(componentRoot, 'dist/cursor')) {
    throw Error('Cursor output must be inside the Arknights tools dist/cursor directory');
  }
  for (let current = destination; current !== path.dirname(current); current = path.dirname(current)) {
    if (fs.existsSync(current) && fs.lstatSync(current).isSymbolicLink()) {
      throw Error('Linked cursor build output');
    }
  }
  const compiler = process.env.QAQR_CSC || path.join(componentRoot, '.cache/roslyn-4.14.0/tasks/net472/csc.exe');
  function run(command, args, options = {}) {
    const result = spawnSync(command, args, { windowsHide: true, encoding: 'utf8', timeout: 180000, ...options });
    if (result.error || result.status !== 0) {
      throw result.error || Error(result.stdout || result.stderr || 'Cursor component build failed');
    }
    return result.stdout;
  }
  if (!fs.existsSync(compiler)) {
    run(process.execPath, [path.join(__dirname, 'tools/prepare-compiler.cjs')]);
  }
  const framework = path.join(process.env.WINDIR || 'C:/Windows', 'Microsoft.NET/Framework64/v4.0.30319');
  const references = ['mscorlib.dll', 'System.dll', 'System.Core.dll', 'System.Web.Extensions.dll'].map(name => '/reference:' + path.join(framework, name));
  const common = ['/nologo', '/noconfig', '/nostdlib+', '/target:exe', '/platform:x64', '/optimize+', '/debug-', '/deterministic+', '/langversion:latest', '/nullable:enable', '/utf8output', '/codepage:65001', '/pathmap:' + __dirname + '=/_/arknights-cursor', ...references];
  const core = fs.readdirSync(path.join(__dirname, 'src/Core')).filter(name => name.endsWith('.cs')).sort().map(name => path.join(__dirname, 'src/Core', name));
  const info = path.join(__dirname, 'qaqr/AssemblyInfo.cs');
  fs.mkdirSync(destination, { recursive: true });
  const executable = path.join(destination, 'ArknightsSystemCursor.Cli.exe');
  run(compiler, [...common, '/out:' + executable, ...core, info, path.join(__dirname, 'src/Cli/Program.cs')]);
  fs.writeFileSync(executable + '.config', '<?xml version="1.0"?><configuration><startup><supportedRuntime version="v4.0" sku=".NETFramework,Version=v4.8"/></startup></configuration>');
  for (const name of ['LICENSE', 'SOURCES.json']) {
    fs.copyFileSync(path.join(__dirname, name), path.join(destination, name));
  }
  if (test) {
    const testDirectory = path.join(__dirname, 'build/tests');
    fs.mkdirSync(testDirectory, { recursive: true });
    const tests = path.join(testDirectory, 'CursorTests.exe');
    run(compiler, [...common, '/out:' + tests, ...core, info, path.join(__dirname, 'tests/Program.cs')]);
    process.stdout.write(run(tests, [], { cwd: testDirectory, timeout: 60000 }));
  }
  const hash = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
  const sources = JSON.parse(fs.readFileSync(path.join(__dirname, 'SOURCES.json'), 'utf8'));
  const files = {};
  for (const name of ['ArknightsSystemCursor.Cli.exe', 'ArknightsSystemCursor.Cli.exe.config', 'LICENSE', 'SOURCES.json']) {
    files[name] = hash(path.join(destination, name));
  }
  fs.writeFileSync(path.join(destination, 'component.json'), JSON.stringify({ schemaVersion: 1, protocol: 1, source: sources.repository, sourceRevision: sources.revision, files }, null, 2));
  console.log('Built source-pinned Arknights system cursor CLI');
  return destination;
}

module.exports = { build };
