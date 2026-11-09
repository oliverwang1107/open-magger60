#!/bin/sh
# Disassemble the 32 KB bootloader that tools/magger60_backup.py dumped from YOUR OWN keyboard.
# The dump is the manufacturer's code and stays on your PC (backup*/ and ghidra/out/ are git-ignored, never publish them).
# usage: ghidra/analyze_boot.sh <bootloader_0x08000000.bin>
# Needs Ghidra (GHIDRA=/path/to/ghidra, default /c/tools/ghidra_12.1.4_PUBLIC) and JDK 21 (JAVA_HOME).
# Git Bash on Windows. Output: ghidra/out/Magger60-boot/listing.txt (disassembly of 0x08000000-0x08007FFF).
set -e
GHIDRA=${GHIDRA:-/c/tools/ghidra_12.1.4_PUBLIC}
export JAVA_HOME=${JAVA_HOME:-"C:\Program Files\Java\jdk-21"}
HERE=$(cd "$(dirname "$0")" && pwd)
BIN=${1:-}
if [ -z "$BIN" ] || [ ! -f "$BIN" ]; then
  echo "usage: ghidra/analyze_boot.sh <bootloader_0x08000000.bin>   (made by tools/magger60_backup.py --run)" >&2
  exit 2
fi
TAG=Magger60-boot
mkdir -p "$HERE/proj" "$HERE/out/$TAG"
rm -f "$HERE/out/$TAG/listing.txt"
"$GHIDRA/support/analyzeHeadless.bat" "$(cygpath -w "$HERE/proj")" Magger60boot \
  -import "$(cygpath -w "$BIN")" -overwrite \
  -processor ARM:LE:32:Cortex -cspec default \
  -loader BinaryLoader -loader-baseAddr 0x08000000 \
  -scriptPath "$(cygpath -w "$HERE/scripts")" \
  -preScript BootPreSetup.java \
  -postScript DumpListing.java "$(cygpath -w "$HERE/out/$TAG/listing.txt")" 0x08000000 0x08007fff \
  -log "$(cygpath -w "$HERE/out/$TAG/ghidra.log")" < /dev/null
echo "wrote $HERE/out/$TAG/listing.txt"
