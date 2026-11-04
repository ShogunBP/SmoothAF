#!/bin/bash
set -e
ProjectRoot=/src
LegacyRoot=/src/docs/references/magic-lantern-branch-lua_fix
SymbolSource=/src/docs/references/magic-lantern-branch-lua_fix/600D_102.SYM

stage=$(mktemp -d -t smoothaf-XXXXXXXX)
echo "stage=$stage"
legacyStage=$stage/legacy
mkdir -p $legacyStage
cp -r $LegacyRoot $legacyStage/
echo "CP LEGACY DONE"

# The legacy tree is now at $legacyStage/magic-lantern-branch-lua_fix
actualLegacy=$legacyStage/magic-lantern-branch-lua_fix

integration=$actualLegacy/modules/smoothaf
mkdir -p $integration
echo "integration=$integration"

cp /src/src/smoothaf.c $integration/
cp /src/src/smoothaf_core.h $integration/
cp /src/src/smoothaf_algorithm.h $integration/
cp /src/src/smoothaf_direction.h $integration/
cp /src/src/smoothaf_recovery.h $integration/
cp /src/src/smoothaf_logger.h $integration/
cp /src/src/smoothaf_logger.c $integration/
cp /src/src/smoothaf_logger_fio.h $integration/
cp /src/src/smoothaf_logger_fio.c $integration/
echo "CP SOURCE DONE"

buildId="$(git -c core.autocrlf=true -C "$ProjectRoot" rev-parse --short=12 HEAD)-600D.102"
if [ -n "$(git -c core.autocrlf=true -C "$ProjectRoot" status --porcelain)" ]; then
    buildId="${buildId}-dirty"
fi
cat > $integration/smoothaf_build_id.h << EOFF
#ifndef SMOOTHAF_BUILD_ID_H
#define SMOOTHAF_BUILD_ID_H
#define SMOOTHAF_BUILD_ID "$buildId"
#define SMOOTHAF_BUILD_CONFIG "600D.102"
static inline const char *smoothaf_build_id_value(void)
{
    return SMOOTHAF_BUILD_ID;
}
#endif
EOFF
echo "BUILD ID=$buildId"

cp /src/src/smoothaf_diagnostics.h $integration/
cp /src/src/smoothaf_monitor.h $integration/
cp /src/src/smoothaf_episode.h $integration/
cp /src/integration/Makefile $integration/
cp /src/integration/README.rst $integration/
cp /src/docker/generate-strings.py $integration/
cp /src/docker/checkdep_py3.py $actualLegacy/modules/
echo "CP MISC DONE"

symbolTarget=$actualLegacy/platform/600D.102/magiclantern.sym
symbolDir=$(dirname $symbolTarget)
mkdir -p $symbolDir
cp $SymbolSource $symbolTarget
echo "SYMBOLS DONE"

cd $actualLegacy/modules/smoothaf
echo "BUILDING..."
make clean all PYTHON=python3
built=$integration/smoothaf.mo
if [ ! -f "$built" ]; then
    echo "ARM build completed without producing smoothaf.mo" >&2
    exit 1
fi
dep=$integration/smoothaf.dep
if [ ! -f "$dep" ]; then
    echo "ARM build completed without producing smoothaf.dep" >&2
    exit 1
fi
sha256sum "$built"
echo "SIZE=$(stat -c '%s' "$built")"
echo "DEP=$(grep -cve '^[[:space:]]*$' "$dep")"
echo "BUILD_ID=$buildId"