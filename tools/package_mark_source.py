"""Create the corresponding source archive included with a MARK release."""
from pathlib import Path
import argparse
import os
import shutil
import subprocess
import zipfile


def create_archive(source, output):
    source = source.resolve(strict=True)
    output = output.resolve()
    if output.is_relative_to(source):
        raise ValueError('Place the output archive outside the source directory')
    for name in ('CMakeLists.txt', 'LICENSE', 'README.md', 'src/main.cpp', 'docs/BUILD.md'):
        if not (source / name).is_file():
            raise ValueError('The source directory is incomplete: ' + name)
    node = shutil.which('node')
    if not node:
        raise ValueError('Node.js is required for release localization validation')
    subprocess.run([node, str(source / 'tools/check_ui_localization.cjs')], check=True)
    skipped = {'.git', '.vs', '.idea', '.vscode', '__pycache__', 'node_modules'}
    root_outputs = {'build', 'build-private', 'out', 'release', 'dist', 'private', 'software', 'knowledge-base', 'runtime-data', 'tmp'}
    files = []
    for path in sorted(source.rglob('*')):
        relative = path.relative_to(source)
        if set(relative.parts) & skipped or relative.parts[0] in root_outputs:
            continue
        if path.is_symlink():
            raise ValueError('Source links are not supported: ' + relative.as_posix())
        if path.is_file():
            if path.name in ('CMakeCache.txt', 'ui_state.ini', 'settings.ini', 'network.ini', '.env') or path.suffix in ('.duckdb', '.wal', '.pdb'):
                raise ValueError('A local configuration or generated file is present: ' + relative.as_posix())
            files.append((path, relative))
    output.parent.mkdir(parents=True, exist_ok=True)
    building = output.with_name(output.name + '.building')
    if building.exists():
        raise FileExistsError('An incomplete source archive already exists')
    try:
        with zipfile.ZipFile(building, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
            for path, relative in files:
                archive.write(path, 'MARK-v1.0.0-source/' + relative.as_posix())
        with zipfile.ZipFile(building) as archive:
            bad = archive.testzip()
            if bad:
                raise ValueError('Source archive verification failed: ' + bad)
        os.replace(building, output)
    finally:
        building.unlink(missing_ok=True)
    return len(files)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--output', type=Path, required=True)
    arguments = parser.parse_args()
    print('Archived', create_archive(arguments.source, arguments.output), 'source files')
