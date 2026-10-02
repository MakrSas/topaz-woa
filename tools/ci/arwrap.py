# llvm-lib wrapper for the Mesa ARM64 cross build: meson passes /MACHINE:x64 to the static linker
# even with an aarch64 host machine; replace it with /MACHINE:ARM64.
import subprocess
import sys

args = [a for a in sys.argv[1:] if not a.upper().startswith('/MACHINE:')]
sys.exit(subprocess.call(['llvm-lib', '/MACHINE:ARM64'] + args))
