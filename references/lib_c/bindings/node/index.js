/**
 * M4 AI Engine — Node.js bindings.
 *
 * Usage:
 *   const { Engine } = require('m4engine');
 *   const engine = new Engine({ mode: 2, debug_modules: ['ai_agent'] });
 *   const reply = engine.chat('default', 'user_1', 'Hello!');
 *   console.log(reply);
 *   engine.close();
 */

const ffi = require('ffi-napi');
const ref = require('ref-napi');
const path = require('path');
const fs = require('fs');
const os = require('os');

function findLib() {
  // Env override
  if (process.env.M4ENGINE_LIB && fs.existsSync(process.env.M4ENGINE_LIB))
    return process.env.M4ENGINE_LIB;

  // Bundled with npm package
  const ext = os.platform() === 'darwin' ? 'dylib' : 'so';
  const bundled = path.join(__dirname, 'native', 'lib', `libm4engine.${ext}`);
  if (fs.existsSync(bundled)) return bundled;

  // Common relative paths
  for (const rel of ['lib', '../lib', '../c-lib/lib', '../../c-lib/lib']) {
    const p = path.join(__dirname, rel, `libm4engine.${ext}`);
    if (fs.existsSync(p)) return p;
  }

  throw new Error(
    'libm4engine not found. Set M4ENGINE_LIB env var or run: npm install (triggers postinstall download)'
  );
}

const lib = ffi.Library(findLib(), {
  api_create:              ['pointer', ['string']],
  api_destroy:             ['void',    ['pointer']],
  api_chat:                ['int',     ['pointer', 'string', 'string', 'string',
                                        'pointer', 'size_t', 'pointer', 'pointer']],
  api_load_chat_history:   ['int',     ['pointer', 'string', 'string']],
});

class Engine {
  /**
   * @param {Object|string|null} opts - JSON options. See docs/api.md.
   */
  constructor(opts = null) {
    const json = typeof opts === 'object' ? JSON.stringify(opts || {}) : (opts || '{}');
    this._ctx = lib.api_create(json);
    if (this._ctx.isNull()) {
      throw new Error('api_create failed — check options and stderr logs');
    }
  }

  close() {
    if (this._ctx && !this._ctx.isNull()) {
      lib.api_destroy(this._ctx);
      this._ctx = null;
    }
  }

  /**
   * Sync chat. Returns reply string or null on error.
   */
  chat(tenantId = 'default', userId = 'default', message = '', bufSize = 32768) {
    const reply = Buffer.alloc(bufSize);
    const rc = lib.api_chat(this._ctx, tenantId, userId, message,
                             reply, bufSize, ref.NULL, ref.NULL);
    return rc === 0 ? reply.toString('utf8').replace(/\0/g, '').trim() : null;
  }

  /**
   * Load chat history from MongoDB.
   */
  loadHistory(tenantId = 'default', userId = 'default') {
    return lib.api_load_chat_history(this._ctx, tenantId, userId);
  }
}

module.exports = { Engine };
