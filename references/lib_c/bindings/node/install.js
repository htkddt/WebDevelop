#!/usr/bin/env node
/**
 * Postinstall: download prebuilt m4engine library from GitHub Releases.
 * Requires: gh CLI authenticated (gh auth login)
 */
const { execSync } = require('child_process');
const fs = require('fs');
const path = require('path');
const os = require('os');

const REPO = 'ngoky/lib_c';
const DEST = path.join(__dirname, 'native');

if (fs.existsSync(path.join(DEST, 'lib'))) {
  console.log('[m4engine] native library already installed');
  process.exit(0);
}

const plat = `${os.platform() === 'darwin' ? 'darwin' : 'linux'}-${os.arch() === 'x64' ? 'amd64' : 'arm64'}`;
console.log(`[m4engine] downloading prebuilt library for ${plat}...`);

try {
  const tmpdir = fs.mkdtempSync(path.join(os.tmpdir(), 'm4engine-'));
  execSync(`gh release download --repo ${REPO} --pattern "*${plat}*" --dir "${tmpdir}"`, { stdio: 'inherit' });

  const tarball = fs.readdirSync(tmpdir).find(f => f.endsWith('.tar.gz'));
  if (!tarball) throw new Error(`No tarball found for ${plat}`);

  fs.mkdirSync(DEST, { recursive: true });
  execSync(`tar xzf "${path.join(tmpdir, tarball)}" --strip-components=1 -C "${DEST}"`, { stdio: 'inherit' });
  fs.rmSync(tmpdir, { recursive: true });

  console.log(`[m4engine] installed to ${DEST}/`);
} catch (e) {
  console.error(`[m4engine] install failed: ${e.message}`);
  console.error('[m4engine] ensure gh CLI is installed and authenticated: gh auth login');
  process.exit(1);
}
