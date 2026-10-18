"""Build a single Windows installer from a prepared MARK distribution.

Requires Python, DuckDB and cryptography on the packaging machine. The
generated installer needs none of these Python dependencies on the user's PC.
"""
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import duckdb
from mark_package import file_hash, seal, unseal

TRAILER = struct.Struct('<8sQQ32sQQQQ')

def installation_space(root, portable):
    unpacked = sum(path.stat().st_size for path in root.rglob('*') if path.is_file())
    installed = 0
    conversions = []
    growth = 0
    for path in root.rglob('*'):
        if not path.is_file():
            continue
        final_size = (portable / path.relative_to(root)).stat().st_size
        installed += final_size
        if path.suffix.lower() == '.duckdb':
            conversions.append(final_size)
            growth += max(0, final_size - path.stat().st_size)
    conversions.sort(reverse=True)
    return {'unpacked': unpacked, 'installed': installed,
            'conversion_extra_one': growth + sum(conversions[:1]),
            'conversion_extra_two': growth + sum(conversions[:2])}

def literal(value):
    return "'" + str(value).replace("'", "''") + "'"

def identifier(value):
    return '"' + value.replace('"', '""') + '"'

def attach_source(connection, source, key):
    try:
        connection.execute('ATTACH ' + literal(source) + ' AS source_db (READ_ONLY)')
    except duckdb.Error as error:
        if 'encrypt' not in str(error).lower() and 'key' not in str(error).lower():
            raise
        connection.execute('ATTACH ' + literal(source) + ' AS source_db (READ_ONLY,ENCRYPTION_KEY ' + literal(key) + ')')

def fingerprints(connection, database):
    tables = connection.execute('SELECT schema_name,table_name FROM duckdb_tables() WHERE database_name=? ORDER BY 1,2', [database]).fetchall()
    result = []
    for schema, table in tables:
        columns = connection.execute('SELECT column_name,data_type FROM duckdb_columns() WHERE database_name=? AND schema_name=? AND table_name=? ORDER BY column_index', [database, schema, table]).fetchall()
        expression = 'hash(' + ','.join(identifier(name) for name, _ in columns) + ')'
        qualified = '.'.join(identifier(value) for value in (database, schema, table))
        values = connection.execute('SELECT count(*),bit_xor(' + expression + '),sum(' + expression + ') FROM ' + qualified).fetchone()
        result.append((schema, table, columns, values))
    return result

def export_database(source, target, key):
    before = file_hash(source)
    target.parent.mkdir(parents=True, exist_ok=True)
    connection = duckdb.connect(config={'threads': 2, 'memory_limit': '768MB'})
    try:
        connection.execute('SET preserve_insertion_order=false')
        attach_source(connection, source, key)
        connection.execute('ATTACH ' + literal(target) + ' AS target_db')
        connection.execute('COPY FROM DATABASE source_db TO target_db')
        connection.execute('CHECKPOINT target_db')
        expected = fingerprints(connection, 'source_db')
        if expected != fingerprints(connection, 'target_db'):
            raise ValueError('Database content verification failed: ' + source.name)
        for function, column in (('duckdb_views()', 'view_name'), ('duckdb_indexes()', 'index_name')):
            predicate = ' AND NOT internal' if function == 'duckdb_views()' else ''
            query = 'SELECT schema_name,' + column + ' FROM ' + function + ' WHERE database_name=?' + predicate + ' ORDER BY 1,2'
            if connection.execute(query, ['source_db']).fetchall() != connection.execute(query, ['target_db']).fetchall():
                raise ValueError('Database catalog verification failed: ' + source.name)
    finally:
        connection.close()
    if file_hash(source) != before:
        raise ValueError('Source database changed during packaging: ' + source.name)
    return {'database': source.name, 'source_sha256': before, 'tables': len(expected), 'bytes': target.stat().st_size}

def main(arguments, key):
    portable = arguments.portable.resolve(strict=True)
    setup = arguments.setup.resolve(strict=True)
    output = arguments.output.resolve()
    if output.exists():
        raise FileExistsError('The output installer already exists')
    if not (portable / 'software/MARK.exe').is_file():
        raise ValueError('The input folder must contain software/MARK.exe')
    if not (portable / 'source/MARK-v1.0.0-source.zip').is_file():
        raise ValueError('The input folder must contain source/MARK-v1.0.0-source.zip; create it with package_mark_source.py')
    if setup.stat().st_size > 100 * 1024**2:
        raise ValueError('Expected an unpackaged MARKSetup.exe executable')
    sources = sorted((portable / 'knowledge-base').rglob('*.duckdb'), key=lambda path: path.stat().st_size, reverse=True)
    if not sources:
        raise ValueError('No DuckDB knowledge bases found')
    arguments.work.parent.mkdir(parents=True, exist_ok=True)
    if shutil.disk_usage(arguments.work.parent.resolve()).free < 50 * 1024**3:
        raise ValueError('Allow 50 GiB of temporary space on the packaging machine')
    arguments.work.mkdir(parents=True, exist_ok=True)
    sevenzip = arguments.sevenzip or portable / 'software/resources/download-tools/7z.exe'
    started = time.monotonic()
    report = {'database_count': len(sources), 'databases': [], 'version': '1.0.0'}
    with tempfile.TemporaryDirectory(prefix='MARK-package-', dir=arguments.work) as name:
        work = Path(name)
        payload = work / 'payload'
        payload.mkdir()
        root = payload / 'MARK'
        for source in portable.rglob('*'):
            if source.is_symlink():
                raise ValueError('File links are not supported in the release folder')
            if not source.is_file():
                continue
            if source.name in ('文件校验清单.json', 'SHA256SUMS.txt', 'MARK.installation.json'):
                continue
            if source.name.lower() in ('ui_state.ini', 'settings.ini', 'network.ini') or source.suffix.lower() in ('.wal', '.log', '.pdb', '.tmp'):
                raise ValueError('A local configuration or development file is present: ' + source.name)
            if 'brand-options' in source.parts or 'private' in source.parts:
                raise ValueError('A private or development folder is present')
            if source.suffix.lower() == '.duckdb':
                continue
            target = root / source.relative_to(portable)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
        with ThreadPoolExecutor(max_workers=2) as pool:
            jobs = {pool.submit(export_database, source, root / source.relative_to(portable), key): source for source in sources}
            try:
                for future in as_completed(jobs):
                    report['databases'].append(future.result())
                    print(json.dumps({'stage': 'databases', 'completed': len(report['databases']), 'total': len(sources)}), flush=True)
            except BaseException:
                for future in jobs:
                    future.cancel()
                raise
        space = installation_space(root, portable)
        report['space_plan'] = space
        archive = work / 'MARK.7z'
        print(json.dumps({'stage': 'compression'}), flush=True)
        with (arguments.work / 'compression.log').open('w', encoding='utf-8') as log:
            code = subprocess.run([str(sevenzip), 'a', '-t7z', '-mx=5', '-mmt=8', '-md=32m', str(archive), 'MARK'], cwd=payload, stdout=log, stderr=subprocess.STDOUT).returncode
        if code:
            raise RuntimeError('Archive creation failed')
        with (arguments.work / 'archive-test.log').open('w', encoding='utf-8') as log:
            code = subprocess.run([str(sevenzip), 't', str(archive)], stdout=log, stderr=subprocess.STDOUT).returncode
        if code:
            raise RuntimeError('Archive verification failed')
        encrypted = work / 'MARK.mkb'
        report.update(seal(archive, encrypted, key, len(sources)))
        verified = work / 'verified.7z'
        unseal(encrypted, verified, key)
        if file_hash(verified) != report['archive_sha256']:
            raise ValueError('Encrypted package round trip failed')
        verified.unlink()
        output.parent.mkdir(parents=True, exist_ok=True)
        building = output.with_name(output.name + '.building')
        if building.exists():
            raise FileExistsError('An incomplete output file already exists')
        payload_hash = file_hash(encrypted)
        with building.open('xb') as stream, setup.open('rb') as stub, encrypted.open('rb') as data:
            shutil.copyfileobj(stub, stream, 4 * 1024 * 1024)
            offset = stream.tell()
            shutil.copyfileobj(data, stream, 4 * 1024 * 1024)
            stream.write(TRAILER.pack(b'MARKSFX2', offset, encrypted.stat().st_size, bytes.fromhex(payload_hash),
                space['unpacked'], space['installed'], space['conversion_extra_one'], space['conversion_extra_two']))
        if building.stat().st_size >= 2 * 1024**3:
            building.unlink()
            raise ValueError('The installer exceeds the GitHub attachment limit')
        os.replace(building, output)
        report.update(installer_bytes=output.stat().st_size, installer_sha256=file_hash(output),
                      encrypted_payload_sha256=payload_hash, stub_bytes=setup.stat().st_size,
                      tables_verified=sum(item['tables'] for item in report['databases']),
                      seconds=round(time.monotonic() - started, 1), passed=True)
        (arguments.work / 'package-report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        print(json.dumps({name: value for name, value in report.items() if name != 'databases'}), flush=True)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--portable', type=Path, required=True)
    parser.add_argument('--setup', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--key-file', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--sevenzip', type=Path)
    arguments = parser.parse_args()
    key = ''
    try:
        key = arguments.key_file.read_text(encoding='utf-8-sig').strip()
        if not re.fullmatch('[0-9A-Fa-f]{64}', key):
            raise ValueError('Use a key file containing 64 hexadecimal characters')
        main(arguments, key)
    except Exception as error:
        text = str(error)
        if key:
            text = text.replace(key, '[redacted]')
        print('Packaging failed: ' + text, file=sys.stderr)
        sys.exit(1)
