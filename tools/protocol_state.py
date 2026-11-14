from pathlib import Path
import hashlib
import sys

ROOT = Path(__file__).resolve().parents[1]
PROTOCOL = ROOT / 'Protocol'

def schema_digest():
    base = PROTOCOL / 'Schema'
    files = sorted(base.rglob('*.proto'))
    if not files:
        raise RuntimeError('No .proto files in Protocol/Schema')
    digest = hashlib.sha256(b'protobuf-v33.6\0')
    for file in files:
        digest.update(file.relative_to(base).as_posix().encode('utf-8') + b'\0')
        digest.update(file.read_bytes())
        digest.update(b'\0')
    return digest.hexdigest()

def stamp(config):
    return PROTOCOL / 'build' / config / 'schema.sha256'

if __name__ == '__main__':
    try:
        config = sys.argv[1]
        if config not in ('Debug', 'Release'):
            raise RuntimeError('Expected Debug or Release')
        saved = stamp(config)
        if not saved.is_file() or saved.read_text(encoding='ascii').strip() != schema_digest():
            raise RuntimeError(f'Protocol schema is newer than {config} library. Run Build_Protocol.bat {config} first.')
    except (IndexError, OSError, RuntimeError) as exc:
        print(f'ERROR: {exc}', file=sys.stderr)
        sys.exit(1)
