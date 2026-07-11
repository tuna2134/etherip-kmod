savedcmd_etherip6.mod := printf '%s\n'   etherip6.o | awk '!x[$$0]++ { print("./"$$0) }' > etherip6.mod
