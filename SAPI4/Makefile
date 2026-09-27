# SAPI 4 engine-in-an-interpreter proof of concept (Mac). Pure C, no JIT.
CC ?= clang
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter
LIB = emu/x86.c emu/emu.c emu/crt.c emu/winapi.c emu/ole.c emu/registry.c emu/sapi4_tts.c
SRC = $(LIB) emu/sapi4_speak.c
HDR = emu/x86.h emu/emu.h emu/emu_internal.h emu/sapi4_tts.h

build/sapi4_speak: $(SRC) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRC) -lm

build/sapi4_speak_asan: $(SRC) $(HDR)
	@mkdir -p build
	$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -o $@ $(SRC) -lm

# differential test build: every instruction cross-checked against Unicorn (brew install unicorn). Mac dev only.
build/sapi4_lockstep: $(SRC) $(HDR) emu/lockstep.c
	@mkdir -p build
	$(CC) -std=c11 -O1 -g -DX86_LOCKSTEP -I/opt/homebrew/include -o $@ $(SRC) emu/lockstep.c -L/opt/homebrew/lib -lunicorn -lm

check: build/sapi4_speak
	./tools/check_refs.sh

# all 19 modes (46 renders) against the recorded hashes - run after any interpreter change
golden: build/sapi4_speak
	./tools/golden.sh

clean:
	rm -rf build

# The SAPI 4 bridge exactly as the iOS targets compile it (emulator + sapi4_tts + cv4_bridge + the SAPI 5
# bridge's sanitizer and trimmer, which pulls in the Sam engine it lives next to).
ROOT = ..
SAM = $(ROOT)/Engine/sam/sam.c $(ROOT)/Engine/sam/sam_lex.c $(ROOT)/Engine/sam/sam_morph.c $(ROOT)/Engine/sam/sam_pos.c \
      $(ROOT)/Engine/sam/sam_norm.c $(ROOT)/Engine/sam/sam_front.c $(ROOT)/Engine/sam/sam4fx.c $(ROOT)/Engine/sam/sam_tts.c
BRIDGE = $(ROOT)/Shared/Bridge/cv_bridge.c $(ROOT)/Shared/Bridge/cv4_bridge.c $(ROOT)/Shared/Bridge/cv_resample.c
TINC = -Iemu -I$(ROOT)/Shared/Bridge -I$(ROOT)/Engine/sam
TDEPS = $(LIB) $(HDR) $(BRIDGE) $(ROOT)/Shared/Bridge/cv4_bridge.h $(ROOT)/Shared/Bridge/cv_bridge.h $(ROOT)/Shared/Bridge/cv_resample.h test/cv4_test.c

build/cv4_test: $(TDEPS)
	@mkdir -p build
	$(CC) -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -DCV_NO_DEBUG_ENV $(TINC) -o $@ $(LIB) $(BRIDGE) $(SAM) test/cv4_test.c -lm -lpthread

build/cv4_test_asan: $(TDEPS)
	@mkdir -p build
	$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -DCV_NO_DEBUG_ENV $(TINC) -o $@ $(LIB) $(BRIDGE) $(SAM) test/cv4_test.c -lm -lpthread

build/cv4_test_tsan: $(TDEPS)
	@mkdir -p build
	$(CC) -O1 -g -fsanitize=thread -DCV_NO_DEBUG_ENV $(TINC) -o $@ $(LIB) $(BRIDGE) $(SAM) test/cv4_test.c -lm -lpthread

test: build/cv4_test
	@mkdir -p samples/selftest samples/selftest_tv
	./build/cv4_test data/msttsl selftest samples/selftest
	./build/cv4_test data/tv_enua selftest samples/selftest_tv

# Stage exactly the files the app bundles (folder resource ../SAPI4Voices in both iOS targets, next to
# VoiceData), after checking every one against the SHA-256 recorded in tools/data.sha256. Gitignored.
# L&H TruVoice is no longer bundled (2026-09-26: the app runs it on OpenTV, Engine/opentv); tv_enua.dll + MSVCP50.dll stay
# in data/tv_enua for `make check golden test` and the old-vs-new comparison (`make truvoice-ab` in the repo root).
bundle:
	@rm -rf ../SAPI4Voices && mkdir -p ../SAPI4Voices
	cd data/msttsl && shasum -a 256 -c ../../tools/data.sha256
	for f in $$(awk '{print $$2}' tools/data.sha256); do cp data/msttsl/$$f ../SAPI4Voices/$$f; done
	cd ../SAPI4Voices && shasum -a 256 -c ../SAPI4/tools/data.sha256 >/dev/null && ls -la
	cd data/tv_enua && shasum -a 256 -c ../../tools/data_tv.sha256

.PHONY: check golden test clean bundle
