// Run with the bundled Node runtime and its artifact-tool node_modules.
import fs from 'node:fs/promises';
import { Workbook, SpreadsheetFile } from '@oai/artifact-tool';

const output = process.argv[2];
if (!output) throw new Error('Usage: build_excel_formula_load.mjs OUTPUT_DIRECTORY [ROWS=64]');
const rows = Number(process.argv[3] || 64), first = 7, last = first + rows - 1;
if (!Number.isInteger(rows) || rows < 1 || rows > 1024) throw new Error('Rows must be 1..1024');
const formulaCount = rows * 10 + 10;
const workbook = Workbook.create();
const sheets = ['Summary', 'Model', 'Inputs'].map(name => workbook.worksheets.add(name));
const [summary, model, inputs] = sheets;
const definition = { rows, first, last, sheets: [], expected: {} };
function values(sheet, range, matrix) { sheet.getRange(range).values = matrix; }
values(inputs, 'A2', [['Deterministic inputs']]);
values(inputs, 'A3:B3', [['Scale', 1]]);
values(inputs, 'D3:E3', [['Discount threshold', 50]]);
values(inputs, 'G3:H3', [['Discount rate', 0.125]]);
values(inputs, 'J3:K3', [['Flag threshold', 4000]]);
values(inputs, 'A5', [['Synthetic data, fixed arithmetic sequence']]);
values(inputs, 'A6:C6', [['ID', 'Units', 'Product']]);
values(inputs, 'G6:H6', [['Product', 'Unit price']]);
const data = Array.from({ length: rows }, (_, i) => [i + 1, ((i + 1) * 17) % 97 + 1, ((i + 1) * 13) % 32 + 1]);
values(inputs, 'A7:C' + last, data);
values(inputs, 'G7:H38', Array.from({ length: 32 }, (_, i) => [i + 1, 5 + (i + 1) * 7]));
values(model, 'A2', [['Dependent calculations']]);
values(model, 'A6:J6', [['ID', 'Quantity', 'Product', 'Unit price', 'Gross', 'Discount', 'Net', 'Running total', 'Root', 'Flag']]);
const formulas = data.map((_, i) => {
  const r = first + i;
  return ["='Inputs'!A" + r, "='Inputs'!B" + r + "*'Inputs'!$B$3", "='Inputs'!C" + r,
    '=VLOOKUP(C' + r + ",Inputs!$G$7:$H$38,2,FALSE)", '=B' + r + '*D' + r,
    '=IF(B' + r + ">=Inputs!$E$3,Inputs!$H$3,0)", '=E' + r + '-ROUND(E' + r + '*F' + r + ',0)',
    i ? '=H' + (r - 1) + '+G' + r : '=G' + r, '=SQRT(G' + r + '+1)', '=IF(G' + r + '>=Inputs!$K$3,1,0)'];
});
model.getRange('A7:J' + last).formulas = formulas;
values(summary, 'A2', [['Excel formula load']]);
values(summary, 'A4:A13', [['Net total'], ['Final running total'], ['Product 1 net'], ['Flagged rows'], ['Mean root'], ['Maximum net'], ['Minimum net'], ['Record count'], ['Gross total'], ['Positive rows']]);
summary.getRange('B4:B13').formulas = [
  '=SUM(Model!G7:G' + last + ')', '=Model!H' + last,
  '=SUMIF(Model!C7:C' + last + ',1,Model!G7:G' + last + ')', '=SUM(Model!J7:J' + last + ')',
  '=AVERAGE(Model!I7:I' + last + ')', '=MAX(Model!G7:G' + last + ')', '=MIN(Model!G7:G' + last + ')',
  '=COUNT(Model!A7:A' + last + ')', '=SUM(Model!E7:E' + last + ')', '=COUNTIF(Model!G7:G' + last + ',">0")'
].map(formula => [formula]);
values(summary, 'A16', [[rows.toLocaleString('en-US') + ' records, ' + formulaCount.toLocaleString('en-US') + ' formulas']]);
values(summary, 'A17', [['Synthetic deterministic inputs']]);
for (const sheet of sheets) {
  sheet.showGridLines = false;
  sheet.getUsedRange().format.font = { name: 'Arial', size: 10, color: '#202020' };
  sheet.getUsedRange().format.rowHeight = 18;
  sheet.getUsedRange().format.columnWidth = 14;
  sheet.getRange('A2').format.font = { name: 'Arial', size: 14, bold: true };
  if (sheet !== summary) {
    const header = sheet === model ? 'A6:J6' : 'A6:C6';
    sheet.getRange(header).format = { fill: '#27374D', font: { name: 'Arial', size: 10, bold: true, color: '#FFFFFF' }, rowHeight: 22 };
    sheet.freezePanes.freezeRows(6);
  }
}
summary.getRange('A1:A18').format.columnWidth = 42;
summary.getRange('B1:B18').format.columnWidth = 20;
summary.getRange('B4:B13').setNumberFormat('#,##0');
summary.getRange('B8').setNumberFormat('0.00');
summary.getRange('B4:B5').format.font = { name: 'Arial', size: 14, bold: true };
inputs.getRange('D1:D4').format.columnWidth = 26;
inputs.getRange('J1:J4').format.columnWidth = 22;
inputs.getRange('A5').format.font = { name: 'Arial', size: 10, italic: true };
inputs.getRange('B3').format.fill = '#FFF2CC';
inputs.getRange('H3').setNumberFormat('0.0%');
model.getRange('F7:F' + last).setNumberFormat('0.0%');
model.getRange('G7:H' + last).setNumberFormat('#,##0');
model.getRange('I7:I' + last).setNumberFormat('0.00');
function expected(scale) {
  let net = 0, gross = 0, product1 = 0, flags = 0, roots = 0, maximum = 0, minimum = Infinity;
  const calculated = data.map(([id, units, product]) => {
    const quantity = units * scale, price = 5 + product * 7, amount = quantity * price;
    const discount = quantity >= 50 ? 0.125 : 0, value = amount - Math.floor(amount * discount + 0.5);
    net += value; gross += amount; if (product === 1) product1 += value;
    flags += value >= 4000 ? 1 : 0; roots += Math.sqrt(value + 1); maximum = Math.max(maximum, value); minimum = Math.min(minimum, value);
    return [id, quantity, product, price, amount, discount, value, net, Math.sqrt(value + 1), value >= 4000 ? 1 : 0];
  });
  return { scale, summary: [net, net, product1, flags, roots / rows, maximum, minimum, rows, gross, rows], calculated };
}
definition.expected = { base: expected(1), changed: expected(2) };
workbook.recalculate();
const observed = summary.getRange('B4:B13').values.flat();
for (let i = 0; i < observed.length; ++i) {
  if (!Number.isFinite(observed[i]) || Math.abs(observed[i] - definition.expected.base.summary[i]) > 1e-7) throw new Error('Summary mismatch at row ' + (i + 4) + ': ' + observed[i]);
}
const errors = await workbook.inspect({ kind: 'match', searchTerm: '#REF!|#DIV/0!|#VALUE!|#NAME\\?|#N/A|#NUM!|#NULL!', options: { useRegex: true, maxResults: 20 }, summary: 'formula error scan' });
await fs.mkdir(output, { recursive: true });
await fs.writeFile(output + '/inspection.txt', errors.ndjson);
for (const sheet of sheets) {
  const range = sheet === summary ? 'A1:C18' : sheet === model ? 'A1:J13' : 'A1:K13';
  const preview = await workbook.render({ sheetName: sheet.name, range, scale: 1.5, format: 'png' });
  await fs.writeFile(output + '/' + sheet.name + '.png', new Uint8Array(await preview.arrayBuffer()));
  const used = sheet.getRange(sheet === summary ? 'A1:C18' : sheet === model ? 'A1:J' + last : 'A1:K' + Math.max(last, 38));
  definition.sheets.push({ name: sheet.name, values: used.values, formulas: used.formulas });
}
await (await SpreadsheetFile.exportXlsx(workbook)).save(output + '/FORMULA.xlsx');
await fs.writeFile(output + '/definition.json', JSON.stringify(definition));
console.log(JSON.stringify({ rows, formulas: formulaCount, summary: observed, output }));
