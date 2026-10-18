"""Authenticated streaming format used by MARK Setup."""
from pathlib import Path
import hashlib
import os
import struct
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

HEADER = struct.Struct('<8sQI32s12s32s')
MAGIC = b'MARKMKB1'
TAG_BYTES = 16

def file_hash(path):
    value = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()

def derive_key(key, salt):
    return HKDF(algorithm=hashes.SHA256(), length=32, salt=salt,
                info=b'MARK compressed knowledge distribution v1').derive(bytes.fromhex(key))

def seal(source, target, key, database_count):
    source, target = Path(source), Path(target)
    if target.exists():
        raise FileExistsError(target)
    salt, nonce = os.urandom(32), os.urandom(12)
    source_hash = file_hash(source)
    header = HEADER.pack(MAGIC, source.stat().st_size, database_count, salt, nonce, bytes.fromhex(source_hash))
    cipher = Cipher(algorithms.AES(derive_key(key, salt)), modes.GCM(nonce)).encryptor()
    cipher.authenticate_additional_data(header)
    with source.open('rb') as stream, target.open('xb') as output:
        output.write(header)
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b''):
            output.write(cipher.update(block))
        output.write(cipher.finalize())
        output.write(cipher.tag)
    return {'archive_bytes': source.stat().st_size, 'archive_sha256': source_hash,
            'encrypted_bytes': target.stat().st_size, 'database_count': database_count}

def unseal(source, target, key):
    source, target = Path(source), Path(target)
    if target.exists():
        raise FileExistsError(target)
    verified = False
    try:
        with source.open('rb') as stream:
            header = stream.read(HEADER.size)
            magic, length, database_count, salt, nonce, expected = HEADER.unpack(header)
            if magic != MAGIC or source.stat().st_size != HEADER.size + length + TAG_BYTES:
                raise ValueError('Invalid MARK package header')
            stream.seek(-TAG_BYTES, 2)
            tag = stream.read(TAG_BYTES)
            stream.seek(HEADER.size)
            cipher = Cipher(algorithms.AES(derive_key(key, salt)), modes.GCM(nonce, tag)).decryptor()
            cipher.authenticate_additional_data(header)
            value = hashlib.sha256()
            remaining = length
            with target.open('xb') as output:
                while remaining:
                    block = stream.read(min(4 * 1024 * 1024, remaining))
                    if not block:
                        raise ValueError('Incomplete MARK package')
                    decoded = cipher.update(block)
                    output.write(decoded)
                    value.update(decoded)
                    remaining -= len(block)
                decoded = cipher.finalize()
                output.write(decoded)
                value.update(decoded)
        if value.digest() != expected or target.stat().st_size != length:
            raise ValueError('MARK package checksum mismatch')
        verified = True
        return {'archive_bytes': length, 'archive_sha256': expected.hex(), 'database_count': database_count}
    finally:
        if not verified and target.exists():
            target.unlink()
