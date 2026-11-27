# P4OS - Mensajes: the protocol's tests, on the computer.
#
#   make -C apps/mensajes/tests test     the crypto and TL against vectors.h
#   make -C apps/mensajes/tests cli      mj_cli, a client in a terminal for
#                                        tools/fake_telegram.py
#
# The binaries go to $(OUT) (the scratchpad of whoever runs it is a good
# place); by default a build/ beside this file, which git ignores.

OUT   ?= build
ROOT  := ../../..
MAIN  := ../main
CC    ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers
CFLAGS += -DMJ_HOST -I$(MAIN) -I. -I$(ROOT)/components/monocypher/include

CORE := $(MAIN)/mj_tl.c $(MAIN)/tl_schema.c $(MAIN)/mj_crypto.c $(MAIN)/mj_port.c $(MAIN)/mj_tinfl.c \
        $(ROOT)/components/monocypher/monocypher.c $(ROOT)/components/monocypher/monocypher-ed25519.c
MTP  := $(MAIN)/mj_mtp.c

.PHONY: test cli vectors

vectors: vectors.h
vectors.h: vectors.py ../tools/mtproto_ref.py ../tools/tl_schema.py ../tools/schema/layer234.tl
	python3 vectors.py > vectors.h

$(OUT)/test_crypto: test_crypto.c vectors.h $(CORE) $(MAIN)/*.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ test_crypto.c $(CORE)

test: $(OUT)/test_crypto
	$(OUT)/test_crypto

$(OUT)/mj_cli: mj_cli.c $(CORE) $(MTP) $(MAIN)/*.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ mj_cli.c $(CORE) $(MTP)

cli: $(OUT)/mj_cli
