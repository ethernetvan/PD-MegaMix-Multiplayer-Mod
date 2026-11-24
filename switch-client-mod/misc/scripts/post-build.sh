#!/bin/bash
set -e

OUT_NSO=${OUT}/${BINARY_NAME}
OUT_NPDM=${OUT}/main.npdm

# Guard: fail if any of our own mp:: symbols are left undefined
# (a missing TU links clean as a shared module but crashes PC=0 at boot).
UNDEF_MP=$(aarch64-none-elf-nm -u -C ${NAME}.elf | grep 'mp::' || true)
if [ -n "$UNDEF_MP" ]; then
    echo "ERROR: undefined mp:: symbols in ${NAME}.elf:"
    echo "$UNDEF_MP"
    exit 1
fi

# Clear older build.
rm -rf ${OUT}

# Create out directory.
mkdir ${OUT}

# Copy build into out
mv ${NAME}.nso ${OUT_NSO}
mv ${NAME}.npdm ${OUT_NPDM}

# Copy ELF to user path if defined.
if [ ! -z $ELF_EXTRACT ]; then
    cp "$NAME.elf" "$ELF_EXTRACT"
fi
