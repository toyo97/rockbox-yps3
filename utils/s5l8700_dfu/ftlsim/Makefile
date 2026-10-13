SRC = ../../../firmware/target/arm/s5l8700/yps3/ftl-yps3.c ftlsim.c
BACKUP ?= ../../../nanddump/backup
ftlsim: $(SRC) *.h
	gcc -O1 -g -Wall -Wno-unused-function -DYPS3_NO_SELFTEST -I. -o $@ $(SRC)
run: ftlsim
	./ftlsim $(BACKUP) $(OPS)
.PHONY: run
