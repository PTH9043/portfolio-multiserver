from pathlib import Path
import argparse
import hashlib
import os
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]

def msbuild():
    base = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'))
    vswhere = base / 'Microsoft Visual Studio/Installer/vswhere.exe'
    if not vswhere.is_file():
        raise RuntimeError('Visual Studio Installer vswhere.exe not found')
    text = subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-requires', 'Microsoft.Component.MSBuild', '-property', 'installationPath'], text=True).strip()
    exe = Path(text) / 'MSBuild/Current/Bin/MSBuild.exe'
    if not exe.is_file():
        raise RuntimeError('Visual Studio MSBuild.exe not found')
    return exe

def main():
    parser = argparse.ArgumentParser(description='Build Core and Server and verify the deployed Core DLL.')
    parser.add_argument('configuration', nargs='?', choices=['Debug', 'Release'], default='Debug')
    args = parser.parse_args()
    build = str(msbuild())
    for project in ['Core/Default/Core.vcxproj', 'Server/Default/Server.vcxproj']:
        subprocess.run([build, str(ROOT / project), '/t:Build', '/m', '/nologo', '/p:Configuration=' + args.configuration, '/p:Platform=x64'], cwd=ROOT, check=True)
    filename = 'CoreD.dll' if args.configuration == 'Debug' else 'Core.dll'
    src = ROOT / 'Core/Bin' / args.configuration.lower() / filename
    dst = ROOT / 'Server/Bin' / args.configuration.lower() / filename
    digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    if not src.is_file() or not dst.is_file() or digest(src) != digest(dst):
        raise RuntimeError('Core DLL is missing or does not match the Server copy')
    print(f'Verified: {src} -> {dst}', flush=True)

if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f'ERROR: {exc}', file=sys.stderr)
        sys.exit(1)
