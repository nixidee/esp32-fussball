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
    const start = new RegExp('^([ \t]*)(?:async )?function ' + name + '\\(', 'm').exec(script);
    if (!start) throw Error('Production function ' + name + ' is missing');
    const lineEnd = script.indexOf('\n', start.index);
    const firstLine = script.slice(start.index, lineEnd);
    if (firstLine.trimEnd().endsWith('}')) return firstLine;
    // A declaration closes at its own indentation level, including private
    // feature functions. Nested blocks remain further indented.
    const end = script.indexOf('\n' + start[1] + '}', start.index);
    if (end < 0) throw Error('Production function ' + name + ' is incomplete');
    return script.slice(start.index, end + 2 + start[1].length);
  }).join('\n');
}

// Test-only lexical probes preserve existing race/geometry assertions without
// adding mutable state or test hooks to the production feature interfaces.
function featureProbes(script) {
  const privateNames = new Set();
  script = script.replace(/function create(CompetitionWizard|NamesEditor|ImageEditor)\([\s\S]*?} \/\/ End private \1 scope\./g, block => {
    const owner = /^function create(\w+)/.exec(block)[1];
    const names = [...block.matchAll(/^[ \t]*(?:async )?function (\w+)\(/gm)].map(m => m[1]).slice(1);
    names.forEach(name => privateNames.add(name));
    const state = owner === 'CompetitionWizard' ? 'wizard: () => wizard' : owner === 'ImageEditor' ? 'ED' : 'names: () => names';
    const at = block.lastIndexOf('return Object.freeze(');
    if (at < 0) throw Error('Missing feature interface: ' + owner);
    return block.slice(0, at) + 'Object.assign(globalThis.__featureTests ||= {}, {' + [...names, state].join(',') + '});\n' + block.slice(at);
  });
  // Controlled test boundaries can replace render/HTTP functions after loading.
  // Production wiring uses direct imports; late-bound adapters exist only here.
  script = script.replace(/const \{[^\n]+\} = create(?:CompetitionWizard|NamesEditor|ImageEditor)\([\s\S]*?\n\}\);/g, wiring => wiring.replace(/^    ([$a-z]\w*)(,?)$/gm, (_, name, comma) => '    ' + name + ': (...args) => ' + name + '(...args)' + comma));
  const publicNames = [...new Set([...script.matchAll(/^(?:async )?function (\w+)\(/gm)].map(m => m[1]))].filter(name => !privateNames.has(name));
  return {script, publicNames};
}
function productionFeature(script, owner) {
  const start = script.indexOf('function create' + owner + '(');
  const scope = script.indexOf('} // End private ' + owner + ' scope.', start);
  const end = script.indexOf(');', scope);
  if (start < 0 || scope < 0 || end < 0) throw Error('Missing production feature: ' + owner);
  return script.slice(start, end + 2);
}
module.exports = {readWebSource, autonomousBoundary, productionFunctions, featureProbes, productionFeature};
