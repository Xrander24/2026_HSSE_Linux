savedcmd_student_ioctl.mod := printf '%s\n'   student_ioctl.o | awk '!x[$$0]++ { print("./"$$0) }' > student_ioctl.mod
