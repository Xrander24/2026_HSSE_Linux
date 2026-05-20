savedcmd_minifs.mod := printf '%s\n'   minifs.o | awk '!x[$$0]++ { print("./"$$0) }' > minifs.mod
