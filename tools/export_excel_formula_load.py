#!/usr/bin/env python3
"""Export the verified artifact-tool definition as Excel 97 BIFF8.

Artifact Tool authors the canonical XLSX; xlwt supplies the missing legacy XLS
format. All formulas stay live. Intended-engine calculation is checked in Excel.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('definition', type=Path)
parser.add_argument('output', type=Path)
parser.add_argument('--dependencies', type=Path, required=True)
args = parser.parse_args()
sys.path.insert(0, str(args.dependencies.resolve()))
import xlwt
import xlrd
import olefile

definition = json.loads(args.definition.read_text())
book = xlwt.Workbook()
worksheets = {data['name']: book.add_sheet(data['name']) for data in definition['sheets']}
normal = xlwt.easyxf('font: name Arial, height 200; align: vert centre')
title = xlwt.easyxf('font: name Arial, height 280, bold on; align: vert centre')
header = xlwt.easyxf('font: name Arial, height 200, bold on, colour white; pattern: pattern solid, fore_colour dark_blue; align: vert centre')
integer = xlwt.easyxf('font: name Arial, height 200; align: vert centre', num_format_str='#,##0')
decimal = xlwt.easyxf('font: name Arial, height 200; align: vert centre', num_format_str='0.00')
percent = xlwt.easyxf('font: name Arial, height 200; align: vert centre', num_format_str='0.0%')
result = xlwt.easyxf('font: name Arial, height 280, bold on; align: vert centre', num_format_str='#,##0')
input_style = xlwt.easyxf('font: name Arial, height 200; pattern: pattern solid, fore_colour light_yellow; align: vert centre')
formula_count = 0
for data in definition['sheets']:
    name = data['name']
    sheet = worksheets[name]
    sheet.show_grid = False
    sheet.calc_mode = 1
    sheet.save_recalc = True
    for column in range(11):
        sheet.col(column).width = 14 * 256
    if name == 'Summary':
        sheet.col(0).width = 42 * 256
        sheet.col(1).width = 20 * 256
    else:
        sheet.panes_frozen = True
        sheet.horz_split_pos = 6
    if name == 'Inputs':
        sheet.col(3).width = 26 * 256
        sheet.col(9).width = 22 * 256
    for row, cells in enumerate(data['values']):
        sheet.row(row).height = 18 * 20
        for column, value in enumerate(cells):
            formula = data['formulas'][row][column]
            if value is None and not formula:
                continue
            style = normal
            if row == 1:
                style = title
            elif name != 'Summary' and row == 5:
                style = header
            elif name == 'Summary' and column == 1 and 3 <= row <= 12:
                style = result if row in (3, 4) else decimal if row == 7 else integer
            elif name == 'Model' and row >= 6:
                style = percent if column == 5 else decimal if column == 8 else integer if column in (6, 7) else normal
            elif name == 'Inputs' and (row, column) == (2, 1):
                style = input_style
            elif name == 'Inputs' and (row, column) == (2, 7):
                style = percent
            if formula:
                formula_count += 1
                value = xlwt.Formula(formula.lstrip('='))
            sheet.write(row, column, value, style)
args.output.parent.mkdir(parents=True, exist_ok=True)
book.save(str(args.output))
with olefile.OleFileIO(str(args.output), write_mode=True) as compound:
    stream = bytearray(compound.openstream('Workbook').read())
    offset = 0
    while offset + 4 <= len(stream):
        kind, length = struct.unpack_from('<HH', stream, offset)
        if kind == 6:
            # xlwt emits empty caches with no calculate-on-open flag. Excel 97
            # otherwise treats these as valid empty results until an edit.
            struct.pack_into('<H', stream, offset + 4 + 14, 2)
        offset += 4 + length
    compound.write_stream('Workbook', bytes(stream))
at = records = 0
while at + 4 <= len(stream):
    kind, length = struct.unpack_from('<HH', stream, at)
    body = stream[at + 4:at + 4 + length]
    if kind == 6:
        if len(body) < 22 or struct.unpack_from('<H', body, 20)[0] == 0:
            raise ValueError('Empty BIFF formula')
        if struct.unpack_from('<H', body, 14)[0] != 2:
            raise ValueError('Formula must calculate on open')
        records += 1
    at += 4 + length
assert records == formula_count == definition['rows'] * 10 + 10
reopened = xlrd.open_workbook(str(args.output))
assert reopened.sheet_names() == ['Summary', 'Model', 'Inputs']
assert reopened.sheet_by_name('Inputs').cell_value(2, 1) == 1
report = {'rows': definition['rows'], 'sheets': reopened.sheet_names(),
          'formulaRecords': records, 'bytes': args.output.stat().st_size,
          'sha256': hashlib.sha256(args.output.read_bytes()).hexdigest(),
          'expectedSummary': definition['expected']['base']['summary'],
          'changedSummary': definition['expected']['changed']['summary']}
args.output.with_suffix('.manifest.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report))
