// Compiles every inline <script> in the dashboard pages, so a syntax error that
// would leave the page blank fails CI instead. Run: node tests/web/check-scripts.js
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const dir = path.join(__dirname, '..', '..', 'main', 'www');
let failed = 0;
for (const name of fs.readdirSync(dir).filter(n => n.endsWith('.html'))) {
  const html = fs.readFileSync(path.join(dir, name), 'utf8');
  const re = /<script(?![^>]*\bsrc=)[^>]*>([\s\S]*?)<\/script>/gi;
  let m, n = 0;
  while ((m = re.exec(html))) {
    n++;
    const line = html.slice(0, m.index).split('\n').length;
    try {
      new vm.Script(m[1], { filename: `${name} (script at line ${line})`, lineOffset: line - 1 });
    } catch (e) {
      failed++;
      console.error(`${name}: script at line ${line}: ${e.message}`);
      console.error((e.stack || '').split('\n').slice(0, 4).join('\n'));
    }
  }
  console.log(`${name}: ${n} script(s) checked`);
}
process.exit(failed ? 1 : 0);
