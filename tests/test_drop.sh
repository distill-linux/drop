#!/bin/sh
set -e

DROP_BIN="/home/foggy/distill/src/drop/drop"
SINK_BIN="/home/foggy/distill/src/sink/sink"
TEST_ROOT="/tmp/drop_test_root"
TEST_REPO="/tmp/drop_test_repo"

echo "=== Starting drop Binary Package Manager Tests ==="
rm -rf "$TEST_ROOT" "$TEST_REPO" /tmp/mock_build
mkdir -p "$TEST_ROOT" "$TEST_REPO" /tmp/mock_build/usr/bin

# 1. Create a mock binary
cat << 'PSTUFF' > /tmp/mock_build/usr/bin/mockbin
#!/bin/sh
echo "Distill mock binary running"
PSTUFF
chmod 755 /tmp/mock_build/usr/bin/mockbin

# 2. Write recipe and build using sink
cat << 'RECP' > /tmp/mock.port
PORT_NAME="mockpkg"
PORT_VERSION="1.0.0"
PORT_RELEASE="1"
PORT_DESC="A mock test package"
BUILD_SYSTEM="custom"
BUILD_DEPS=""
RUN_DEPS=""

BUILD:
    mkdir -p $PKG_FAKEROOT/usr/bin
    cp /tmp/mock_build/usr/bin/mockbin $PKG_FAKEROOT/usr/bin/mockbin
    chmod 755 $PKG_FAKEROOT/usr/bin/mockbin
RECP

"$SINK_BIN" --out "$TEST_REPO" make /tmp/mock.port

PKG_FILE="$TEST_REPO/mockpkg-1.0.0.drop"
if [ ! -f "$PKG_FILE" ]; then
    echo "ERROR: Package file $PKG_FILE not created"
    exit 1
fi

# Calculate sha256 and size
SHA=$(sha256sum "$PKG_FILE" | awk '{print $1}')
SIZE=$(wc -c < "$PKG_FILE" | tr -d ' ')

# 3. Create repository index.tsv
printf "mockpkg\t1.0.0\t%s\t%s\tmockpkg-1.0.0.drop\tlibc\n" "$SHA" "$SIZE" > "$TEST_REPO/index.tsv"

echo "--> Testing 'drop in <pkg.drop>'"
"$DROP_BIN" -r "$TEST_ROOT" in "$PKG_FILE"

echo "--> Verifying installed files and database state"
test -f "$TEST_ROOT/usr/bin/mockbin"
test -f "$TEST_ROOT/var/db/drop/ports/mockpkg/.PORT"

echo "--> Testing 'drop ls' (and alias 'drop list')"
LIST_OUT=$("$DROP_BIN" -r "$TEST_ROOT" ls)
echo "$LIST_OUT" | grep "mockpkg"
"$DROP_BIN" -r "$TEST_ROOT" list | grep "mockpkg"

echo "--> Testing 'drop info mockpkg'"
INFO_OUT=$("$DROP_BIN" -r "$TEST_ROOT" info mockpkg)
echo "$INFO_OUT" | grep "Version:      1.0.0"
echo "$INFO_OUT" | grep "Description:  A mock test package"

echo "--> Testing 'drop check mockpkg' (integrity audit against SHA-256 in .PORT)"
"$DROP_BIN" -r "$TEST_ROOT" check mockpkg
"$DROP_BIN" -r "$TEST_ROOT" check

echo "--> Tampering with installed file to test 'drop check' corruption detection"
echo "corrupted content" >> "$TEST_ROOT/usr/bin/mockbin"
if "$DROP_BIN" -r "$TEST_ROOT" check mockpkg; then
    echo "ERROR: drop check should have failed on corrupted file!"
    exit 1
else
    echo "--> drop check correctly detected file corruption!"
fi

echo "--> Testing 'drop rm mockpkg' (zero-query uninstallation)"
"$DROP_BIN" -r "$TEST_ROOT" rm mockpkg

echo "--> Verifying clean uninstallation"
if [ -f "$TEST_ROOT/usr/bin/mockbin" ]; then
    echo "ERROR: Installed file was not removed"
    exit 1
fi
if [ -d "$TEST_ROOT/var/db/drop/ports/mockpkg" ]; then
    echo "ERROR: Database directory was not removed"
    exit 1
fi

echo "--> Testing 'drop in from repository index.tsv with on-the-fly SHA-256 verification'"
"$DROP_BIN" -r "$TEST_ROOT" --repo "$TEST_REPO" in mockpkg
test -f "$TEST_ROOT/usr/bin/mockbin"
test -f "$TEST_ROOT/var/db/drop/ports/mockpkg/.PORT"

echo "--> Testing repository catalog update ('drop update')"
"$DROP_BIN" -r "$TEST_ROOT" --repo "$TEST_REPO" update

echo "--> Testing repository SHA-256 mismatch rejection"
"$DROP_BIN" -r "$TEST_ROOT" rm mockpkg
# Corrupt SHA in index.tsv
printf "mockpkg\t1.0.0\tffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff\t%s\tmockpkg-1.0.0.drop\tlibc\n" "$SIZE" > "$TEST_REPO/index.tsv"
if "$DROP_BIN" -r "$TEST_ROOT" --repo "$TEST_REPO" in mockpkg; then
    echo "ERROR: drop installed package despite SHA-256 mismatch!"
    exit 1
else
    echo "--> Correctly caught repository SHA-256 mismatch and rejected package!"
fi

echo "=== All drop Integration Tests Passed! ==="
