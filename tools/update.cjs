const { spawnSync } = require('node:child_process');
const path = require('node:path');
const root = path.resolve(__dirname, '..');
const identity = ['-c', 'user.name=qaq-revival-publisher[bot]', '-c', 'user.email=338412940+qaq-revival-publisher[bot]@users.noreply.github.com', '-c', 'credential.helper=', '-c', 'core.askPass=', '-c', 'http.https://github.com/.extraheader='];
const env = { ...process.env, GIT_TERMINAL_PROMPT: '0', GCM_INTERACTIVE: 'never' };
function git(args, cwd = root) {
  const result = spawnSync('git', [...identity, ...args], { cwd, env, windowsHide: true, encoding: 'utf8' });
  if (result.status) throw Error(result.stderr || 'Submodule update failed');
  return result.stdout.trim();
}
const branches = require('../sources.json').submoduleBranches;
for (const name of Object.keys(branches)) {
  const directory = path.join(root, 'vendor', name);
  if (require('node:fs').existsSync(path.join(directory, '.git')) && git(['status', '--porcelain'], directory)) throw Error('Save local changes before updating ' + name);
}
git(['submodule', 'update', '--init', '--remote', '--recursive']);
git(['lfs', 'pull'], path.join(root, 'vendor/UnityPy'));
console.log(git(['submodule', 'status', '--recursive']));
