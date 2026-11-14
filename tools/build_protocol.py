from pathlib import Path
import argparse
import base64
import hashlib
import io
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
from protocol_state import schema_digest, stamp

ROOT = Path(__file__).resolve().parents[1]
PROTOCOL = ROOT / 'Protocol'
SOURCE = PROTOCOL / 'vendor/protobuf'
BUILD = PROTOCOL / 'build'
SCHEMA = PROTOCOL / 'Schema'
GENERATED = PROTOCOL / 'Generated'
TAG = 'v33.6'
COMMIT = '6e1998413a5bca7c058b85999667893f167434bc'

def run(args, **kwargs):
    print('>', subprocess.list2cmdline([str(a) for a in args]), flush=True)
    subprocess.run([str(a) for a in args], check=True, **kwargs)

def find_cmake():
    found = shutil.which('cmake')
    if found:
        return found
    from update_core_server import msbuild
    vs = msbuild().parents[3]
    found = vs / 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
    if not found.is_file():
        raise RuntimeError('CMake not found: install the Visual Studio C++ CMake tools')
    return str(found)

def get_protoc():
    exe = PROTOCOL / 'vendor/protoc-33.6/bin/protoc.exe'
    if not exe.is_file():
        url = 'https://api.nuget.org/v3-flatcontainer/google.protobuf.tools/3.33.6/google.protobuf.tools.3.33.6.nupkg'
        # SHA-512 published in NuGet's package response metadata.
        expected = 'qUVAo8zMaaQCGD4kBm2GLCEeGwrxtTHmeQORJ5n80x1qu5dhgmev/EJyO3vsOOdlRSt+1Opp9O3eGFvwsntiTw=='
        with urllib.request.urlopen(url, timeout=120) as response:
            data = response.read()
        if base64.b64encode(hashlib.sha512(data).digest()).decode('ascii') != expected:
            raise RuntimeError('Downloaded protoc archive checksum mismatch')
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            matches = [name for name in archive.namelist() if name.lower().endswith('windows_x64/protoc.exe')]
            if len(matches) != 1:
                raise RuntimeError('Could not locate x64 protoc in the official tools package')
            payload = archive.read(matches[0])
        exe.parent.mkdir(parents=True, exist_ok=True)
        exe.write_bytes(payload)
    result = subprocess.check_output([str(exe), '--version'], text=True).strip()
    if result != 'libprotoc 33.6':
        raise RuntimeError(f'Unexpected protoc version: {result}')
    return exe

def main():
    parser = argparse.ArgumentParser(description='Generate C++ and C# protocol sources and build the C++ library.')
    parser.add_argument('configuration', nargs='?', default='Debug', choices=['Debug', 'Release'])
    args = parser.parse_args()
    files = sorted(SCHEMA.rglob('*.proto'))
    if not files:
        raise RuntimeError('No .proto files found under Protocol/Schema')
    start_digest = schema_digest()
    stamp(args.configuration).unlink(missing_ok=True)
    if not SOURCE.exists():
        SOURCE.parent.mkdir(parents=True, exist_ok=True)
        run(['git', 'clone', '--branch', TAG, '--depth', '1', '--recurse-submodules', '--shallow-submodules', 'https://github.com/protocolbuffers/protobuf.git', SOURCE])
    actual = subprocess.check_output(['git', '-C', str(SOURCE), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != COMMIT:
        raise RuntimeError(f'Unexpected protobuf source revision: {actual}; expected {COMMIT}')
    cmake = find_cmake()
    protoc = get_protoc()
    # Generate into a temporary directory first; do not replace good output if protoc fails.
    with tempfile.TemporaryDirectory(prefix='protocol_generate_', dir=PROTOCOL) as temp:
        stage = Path(temp)
        cpp, cs = stage / 'Cpp', stage / 'CSharp'
        cpp.mkdir(); cs.mkdir()
        run([protoc, '-I', 'Schema', '-I', 'vendor/protobuf/src', '--cpp_out=' + cpp.relative_to(PROTOCOL).as_posix(), '--csharp_out=' + cs.relative_to(PROTOCOL).as_posix(), *[str(f.relative_to(PROTOCOL)) for f in files]], cwd=PROTOCOL)
        for name in ('Cpp', 'CSharp'):
            dest = GENERATED / name
            dest.mkdir(parents=True, exist_ok=True)
            sources = [item for item in (stage / name).rglob('*') if item.is_file()]
            wanted = {item.relative_to(stage / name) for item in sources}
            # These output directories contain generated files only.
            for pattern in (('*.pb.h', '*.pb.cc') if name == 'Cpp' else ('*.cs',)):
                for old in dest.rglob(pattern):
                    if old.relative_to(dest) not in wanted:
                        old.unlink()
            for item in sources:
                target = dest / item.relative_to(stage / name)
                target.parent.mkdir(parents=True, exist_ok=True)
                if not target.is_file() or target.read_bytes() != item.read_bytes():
                    shutil.copy2(item, target)
    run([cmake, '-S', PROTOCOL, '-B', BUILD, '-A', 'x64'])
    run([cmake, '--build', BUILD, '--config', args.configuration, '--target', 'GameProtocol', '--parallel'])
    if schema_digest() != start_digest:
        raise RuntimeError('Schema changed during the build; rerun the protocol build')
    stamp(args.configuration).write_text(start_digest + '\n', encoding='ascii')
    print('Generated C++ and C# from:', *(str(p.relative_to(SCHEMA)) for p in files), sep='\n  ')
    print('Built GameProtocol and protobuf runtime:', args.configuration, flush=True)

if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f'ERROR: {exc}', file=sys.stderr)
        sys.exit(1)
