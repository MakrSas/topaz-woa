# Static-library wrapper for the Mesa ARM64 cross build. With a non-lib name meson calls it GNU-ar
# style ("csr out.a objs..."); with lib it passed /MACHINE:x64. Always run llvm-lib /MACHINE:ARM64.
import subprocess
import sys

args = sys.argv[1:]
if args and not args[0].startswith('/') and not args[0].startswith('@') and not args[0].lower().endswith(('.obj', '.a', '.lib')):
    # GNU ar: <ops> <archive> <members...>
    out, members = args[1], args[2:]
    args = ['/OUT:' + out] + members
args = [a for a in args if not a.upper().startswith('/MACHINE:')]
sys.exit(subprocess.call(['llvm-lib', '/NOLOGO', '/MACHINE:ARM64'] + args))
