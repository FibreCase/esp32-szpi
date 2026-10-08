"""Build a deterministic gzip asset from the self-contained portal."""
import gzip
import pathlib
import sys
pathlib.Path(sys.argv[2]).write_bytes(gzip.compress(pathlib.Path(sys.argv[1]).read_bytes(), compresslevel=9, mtime=0))
