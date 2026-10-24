const fs = require('node:fs'), path = require('node:path'), { spawnSync } = require('node:child_process');
function build({ output, test = false } = {}) {
  if (!output || !path.isAbsolute(output)) throw Error('An absolute generated component output directory is required');
  const destination = path.resolve(output);
  if (path.basename(destination) !== 'arknights-tools' || path.basename(path.dirname(destination)) !== 'local-components') throw Error('Output must be local-components/arknights-tools');
  for (let current = destination; current !== path.dirname(current); current = path.dirname(current)) if (fs.existsSync(current) && fs.lstatSync(current).isSymbolicLink()) throw Error('Linked output directory');
  const result = spawnSync(process.execPath, [path.join(__dirname, 'tools/build.cjs'), ...(test ? ['--test'] : [])], { cwd: __dirname, windowsHide: true, stdio: 'inherit' });
  if (result.error || result.status) throw result.error || Error('Arknights component build failed');
  // This directory contains generated program files only. User data is elsewhere.
  for (const name of ['Mods', 'Resources', 'user-data', 'config.ini']) if (fs.existsSync(path.join(destination, name))) throw Error('Output contains user data');
  fs.rmSync(destination, { recursive: true, force: true });
  fs.cpSync(path.join(__dirname, 'dist'), destination, { recursive: true });
  return destination;
}
module.exports = { build };
