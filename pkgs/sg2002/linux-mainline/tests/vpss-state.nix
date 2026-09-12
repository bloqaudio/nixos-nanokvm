{ runCommand, stdenv, gitMinimal, python3 }:
runCommand "sg2002-vpss-state-tests" {
  nativeBuildInputs = [ stdenv.cc gitMinimal python3 ];
} ''
  git init -q
  for p in ${../patches}/*.patch; do
    if grep -q '^+++ b/drivers/media/platform/sophgo/sg2002-vpss.c$' "$p"; then
      git apply --include=drivers/media/platform/sophgo/sg2002-vpss.c "$p"
    fi
  done
  python3 ${./prepare-vpss-state.py} drivers/media/platform/sophgo/sg2002-vpss.c \
    ${./vpss-state.c} > test-vpss.c
  $CC -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -Wno-sign-compare -fsanitize=address,undefined -fno-omit-frame-pointer \
    test-vpss.c -o test-vpss
  ./test-vpss
  touch "$out"
''
