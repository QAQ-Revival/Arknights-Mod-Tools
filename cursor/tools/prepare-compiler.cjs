const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { spawnSync } = require('node:child_process');
const componentRoot = path.resolve(__dirname, '../..');
const destination = path.join(componentRoot, '.cache/roslyn-4.14.0');

async function main() {
  if (fs.existsSync(path.join(destination, 'tasks/net472/csc.exe'))) {
    return;
  }
  const url = 'https://api.nuget.org/v3-flatcontainer/microsoft.net.compilers.toolset/4.14.0/microsoft.net.compilers.toolset.4.14.0.nupkg';
  const response = await fetch(url, { signal: AbortSignal.timeout(120000) });
  if (!response.ok) {
    throw Error('Official C# compiler download failed: ' + response.status);
  }
  const bytes = Buffer.from(await response.arrayBuffer());
  fs.mkdirSync(destination, { recursive: true });
  const archive = path.join(componentRoot, '.cache/roslyn-4.14.0.zip');
  fs.writeFileSync(archive, bytes);
  // Windows' ZIP support keeps this component build independent of the host app.
  const script = `$ErrorActionPreference = 'Stop'; Add-Type -AssemblyName System.IO.Compression.FileSystem;
    $root = [IO.Path]::GetFullPath($env:QAQR_COMPILER_DIRECTORY);
    $zip = [IO.Compression.ZipFile]::OpenRead($env:QAQR_COMPILER_ARCHIVE);
    try {
      foreach ($entry in $zip.Entries) {
        $target = [IO.Path]::GetFullPath([IO.Path]::Combine($root, $entry.FullName));
        if (-not $target.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Compiler archive escaped its cache directory' };
      }
      foreach ($entry in $zip.Entries) {
        $target = [IO.Path]::GetFullPath([IO.Path]::Combine($root, $entry.FullName));
        if ([string]::IsNullOrEmpty($entry.Name)) { [IO.Directory]::CreateDirectory($target) | Out-Null }
        else { [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target)) | Out-Null; [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $true) }
      }
    } finally { $zip.Dispose() }`;
  const extracted = spawnSync('powershell.exe', ['-NoProfile', '-NonInteractive', '-Command', script], { windowsHide: true, encoding: 'utf8', env: { ...process.env, QAQR_COMPILER_ARCHIVE: archive, QAQR_COMPILER_DIRECTORY: destination } });
  if (extracted.error || extracted.status !== 0) throw extracted.error || Error(extracted.stderr || 'Compiler extraction failed');
  fs.writeFileSync(path.join(destination, 'download.json'), JSON.stringify({ url, sha256: crypto.createHash('sha256').update(bytes).digest('hex') }, null, 2));
}
main().catch(error => {
  console.error(error.message);
  process.exitCode = 1;
});
