'use strict';

// Run the same Python bundler as the firmware; tests never maintain a second
// module list or a second stylesheet/script inlining implementation.
const path = require('node:path');
const {execFileSync} = require('node:child_process');
const bundler = path.resolve(__dirname, '../embed_web.py');
const defaultSource = path.resolve(__dirname, '../../web/index.html');

function readWebSource(source = defaultSource) {
  const html = execFileSync('python3', [bundler, source, '--document'], {
    encoding: 'utf8',
    maxBuffer: 4 * 1024 * 1024,
  });
  const script = html.match(/<script>([\s\S]*?)<\/script>/)?.[1];
  if (!script) throw Error('Bundled production script is missing');
  return {html, script, boundary: autonomousBoundary(script)};
}

function autonomousBoundary(script) {
  const boundary = script.indexOf('// Autonomous work starts here.');
  // Retained evidence snapshots can contain the earlier inline document.
  const legacyBoundary = script.lastIndexOf("\nsetInterval(()=>load('status',false),5000)");
  return boundary >= 0 ? boundary : legacyBoundary;
}

function productionFunctions(script, names) {
  return names.map(name => {
    const start = new RegExp('^(?:async )?function ' + name + '\\(', 'm').exec(script);
    if (!start) throw Error('Production function ' + name + ' is missing');
    const lineEnd = script.indexOf('\n', start.index);
    const firstLine = script.slice(start.index, lineEnd);
    if (firstLine.trimEnd().endsWith('}')) return firstLine;
    // The source formatter leaves every top-level function's closing brace
    // in column zero. Nested functions and blocks remain indented.
    const end = script.indexOf('\n}', start.index);
    if (end < 0) throw Error('Production function ' + name + ' is incomplete');
    return script.slice(start.index, end + 2);
  }).join('\n');
}

module.exports = {readWebSource, autonomousBoundary, productionFunctions};
