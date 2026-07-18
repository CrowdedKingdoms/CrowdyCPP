#!/usr/bin/env node
/**
 * Generate the committed C++ GraphQL surface from:
 *   - operations/<domain>/*.graphql  ->  src/generated/operations.hpp
 *   - schema.gql (enums)             ->  src/generated/enums.hpp
 *
 * Output is committed so external builds never run this script.
 * Usage: node scripts/codegen.mjs
 */
import { readFileSync, writeFileSync, readdirSync, statSync, mkdirSync } from 'node:fs';
import { resolve, dirname, join, basename } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const opsDir = join(root, 'operations');
const outDir = join(root, 'src', 'generated');
mkdirSync(outDir, { recursive: true });

const HEADER = `// GENERATED FILE — do not edit by hand.
// Regenerate with: node scripts/codegen.mjs
// Inputs: operations/**/*.graphql and schema.gql (synced from the published
// SDLs at https://docs.crowdedkingdoms.com/schema/).
`;

// ---------------------------------------------------------------------------
// Operations
// ---------------------------------------------------------------------------

function listDomains() {
  return readdirSync(opsDir)
    .filter((d) => statSync(join(opsDir, d)).isDirectory())
    .sort();
}

function operationsInFile(text) {
  const ops = [];
  const re = /^\s*(query|mutation|subscription)\s+([A-Za-z0-9_]+)/gm;
  let m;
  while ((m = re.exec(text)) !== null) ops.push({ kind: m[1], name: m[2] });
  return ops;
}

let opsHpp = `${HEADER}
#pragma once

#include <string_view>

/// GraphQL operation documents, one namespace per domain. Each constant is
/// the full document text of its source file (which may contain fragments or
/// several operations); the matching *OperationName constant names the
/// operation to execute.
namespace crowdy::gen {

`;

const manifest = [];
for (const domain of listDomains()) {
  opsHpp += `namespace ${domain} {\n\n`;
  const files = readdirSync(join(opsDir, domain))
    .filter((f) => f.endsWith('.graphql'))
    .sort();
  for (const file of files) {
    const text = readFileSync(join(opsDir, domain, file), 'utf8').trim();
    const ops = operationsInFile(text);
    if (ops.length === 0) continue;
    const base = basename(file, '.graphql');
    // Guard against raw-string delimiter collisions.
    if (text.includes(')gql"')) throw new Error(`raw-string delimiter clash in ${file}`);
    opsHpp += `/// ${domain}/${file}\n`;
    opsHpp += `inline constexpr std::string_view k${base}Document = R"gql(${text})gql";\n`;
    for (const op of ops) {
      opsHpp += `inline constexpr std::string_view k${op.name}OperationName = "${op.name}";\n`;
      manifest.push({ domain, file, ...op });
    }
    opsHpp += '\n';
  }
  opsHpp += `}  // namespace ${domain}\n\n`;
}
opsHpp += '}  // namespace crowdy::gen\n';
writeFileSync(join(outDir, 'operations.hpp'), opsHpp);

// ---------------------------------------------------------------------------
// Enums from schema.gql
// ---------------------------------------------------------------------------

const schema = readFileSync(join(root, 'schema.gql'), 'utf8');
const enums = [];
{
  const re = /(?:^|\n)enum\s+([A-Za-z0-9_]+)\s*\{([\s\S]*?)\n\}/g;
  let m;
  while ((m = re.exec(schema)) !== null) {
    const name = m[1];
    // Strip block docstrings and single-line strings before extracting values.
    const body = m[2].replace(/"""[\s\S]*?"""/g, ' ').replace(/"[^"\n]*"/g, ' ');
    const values = [];
    for (const rawLine of body.split('\n')) {
      const line = rawLine.trim();
      if (!line) continue;
      const vm = line.match(/^([A-Za-z_][A-Za-z0-9_]*)\s*$/);
      if (vm) values.push(vm[1]);
    }
    if (values.length > 0 && new Set(values).size === values.length) {
      enums.push({ name, values });
    } else if (values.length > 0) {
      throw new Error(`enum ${name}: duplicate or malformed values parsed: ${values}`);
    }
  }
}

// C++ keyword-safe identifier for an enum value.
const CPP_KEYWORDS = new Set(['default', 'delete', 'new', 'private', 'public', 'protected',
  'register', 'template', 'this', 'true', 'false', 'nullptr', 'operator', 'export', 'import']);
const cppId = (v) => (CPP_KEYWORDS.has(v) || /^\d/.test(v) ? `${v}_` : v);

let enumsHpp = `${HEADER}
#pragma once

#include <optional>
#include <string_view>

/// GraphQL enums from the published schema. Values keep their wire spelling;
/// toString/fromString convert between the enum and the GraphQL string.
namespace crowdy::gen {

`;
for (const e of enums.sort((a, b) => a.name.localeCompare(b.name))) {
  enumsHpp += `enum class ${e.name} {\n`;
  for (const v of e.values) enumsHpp += `  ${cppId(v)},\n`;
  enumsHpp += `};\n\n`;
  enumsHpp += `inline constexpr std::string_view toString(${e.name} v) {\n  switch (v) {\n`;
  for (const v of e.values) enumsHpp += `    case ${e.name}::${cppId(v)}: return "${v}";\n`;
  enumsHpp += `  }\n  return "";\n}\n\n`;
  const fn = `${e.name[0].toLowerCase()}${e.name.slice(1)}FromString`;
  enumsHpp += `inline std::optional<${e.name}> ${fn}(std::string_view s) {\n`;
  for (const v of e.values)
    enumsHpp += `  if (s == "${v}") return ${e.name}::${cppId(v)};\n`;
  enumsHpp += `  return std::nullopt;\n}\n\n`;
}
enumsHpp += '}  // namespace crowdy::gen\n';
writeFileSync(join(outDir, 'enums.hpp'), enumsHpp);

console.log(
  `generated ${manifest.length} operations across ${listDomains().length} domains, ` +
  `${enums.length} enums -> src/generated/`,
);
