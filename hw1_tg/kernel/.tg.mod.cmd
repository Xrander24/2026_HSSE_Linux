savedcmd_tg.mod := printf '%s\n'   tg.o | awk '!x[$$0]++ { print("./"$$0) }' > tg.mod
