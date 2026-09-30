, := ,# a literal comma for $(call ...) arguments (the TruVoice targets below)
# Mac-side build of the engine + bridge exactly as the iOS targets compile them (for tests and samples).
#   make test      -> build/mac/cv_test (with AddressSanitizer + UBSan) and run the self-test
#   make samples   -> render samples/*.wav for every shipped voice
CC ?= cc
ENGINE = Engine/sam/sam.c Engine/sam/sam_lex.c Engine/sam/sam_morph.c Engine/sam/sam_pos.c \
         Engine/sam/sam_norm.c Engine/sam/sam_front.c Engine/sam/sam4fx.c Engine/sam/sam_tts.c
BRIDGE = Shared/Bridge/cv_bridge.c
CFLAGS ?= -std=c99 -O1 -g -Wall -Wextra -Wno-unused-parameter -DCV_NO_DEBUG_ENV \
          -IEngine/sam -IShared/Bridge -fsanitize=address,undefined -fno-omit-frame-pointer
DATA ?= VoiceData

build/mac/cv_test: $(ENGINE) $(BRIDGE) Engine/test/cv_test.c Shared/Bridge/cv_bridge.h Shared/Bridge/cv_fold_table.h
	mkdir -p build/mac
	$(CC) $(CFLAGS) $(ENGINE) $(BRIDGE) Engine/test/cv_test.c -lm -o $@

test: build/mac/cv_test
	mkdir -p samples/selftest
	./build/mac/cv_test $(DATA) selftest samples/selftest

samples: build/mac/cv_test build/mac/anna_test build/mac/onecore_test
	sh tools/render_samples.sh

clean:
	rm -rf build/mac

.PHONY: test samples clean

# Singing mode of the SAPI 5 voices (Engine/test/sing_test.c): the engine + bridge as the iOS targets compile them.
#   make sing-test   ASan/UBSan: every kind of SAPI 5 voice sings, one note per syllable (literal-score counts),
#                    deterministic, transpose / vibrato / rate, hostile text, 20,000 letters, cancel, speech after
#                    singing, empty text; WAVs in samples/sing
#   make sing-tsan   two voices singing on two threads under ThreadSanitizer, identical to each alone
build/mac/sing_test: $(ENGINE) $(BRIDGE) Engine/test/sing_test.c Shared/Bridge/cv_bridge.h Engine/sam/sam.h
	mkdir -p build/mac
	$(CC) $(CFLAGS) $(ENGINE) $(BRIDGE) Engine/test/sing_test.c -lm -lpthread -o $@

build/mac/sing_test_tsan: $(ENGINE) $(BRIDGE) Engine/test/sing_test.c Shared/Bridge/cv_bridge.h Engine/sam/sam.h
	mkdir -p build/mac
	$(CC) -std=c99 -O1 -g -DCV_NO_DEBUG_ENV -IEngine/sam -IShared/Bridge -fsanitize=thread $(ENGINE) $(BRIDGE) \
	  Engine/test/sing_test.c -lm -lpthread -o $@

sing-test: build/mac/sing_test
	mkdir -p samples/sing
	./build/mac/sing_test $(DATA) selftest samples/sing

sing-tsan: build/mac/sing_test_tsan
	./build/mac/sing_test_tsan $(DATA) threads 2

.PHONY: sing-test sing-tsan

# The extension's SSML handling and pauses (Tests/ssml): the same Swift files and C engines/bridges the
# iOS targets compile, built for the Mac. `make ssml-test` = parser fixtures + capture privacy, then
# every fixture rendered before/after through one voice per engine (WAVs in samples/ssml).
# Microsoft Anna's engine (Engine/anna): its flags and own targets are further down.
ANNA = Engine/anna/anna_tts.c Engine/anna/anna_voice.c Engine/anna/anna_render.c Engine/anna/anna_dec_wmav.c \
       Engine/anna/anna_norm.c Engine/anna/anna_lex.c Engine/anna/anna_lts.c Engine/anna/anna_morph.c \
       Engine/anna/anna_pos.c Engine/anna/anna_prosody.c Engine/anna/anna_units.c
# Microsoft David / Zira / Mark (Engine/onecore): its flags and own targets are further down.
ONECORE = $(sort $(wildcard Engine/onecore/*.c))
# L&H TruVoice on OpenTV (Engine/opentv): its flags and own targets are further down.
OPENTV = $(sort $(wildcard Engine/opentv/src/engine/*.c)) Engine/opentv/src/port/tvtts.c Engine/opentv/src/port/msvcrt.c \
         Engine/opentv/src/port/stubs.c
OPENTVFLAGS = -std=gnu11 -fwrapv -fno-strict-aliasing -w -DCV_NO_DEBUG_ENV -IEngine/opentv/src -IEngine/opentv/include \
              -IEngine/opentv/generated
TVDATA = TruVoiceData/tvdata.s
# Microsoft's SAPI 4 engine: the app's (Engine/sapi4, the engine decompiled to C; its flags: see "Microsoft's SAPI 4 engine
# as native C" further down) and the old path (SAPI4/emu, msttssyn.dll in the x86 interpreter: tests and make sapi4-ab only)
SAPI4N = Engine/sapi4/lib/sapi4tts.c Engine/sapi4/lib/stubs.c Engine/sapi4/port/dllimage.c $(sort $(wildcard Engine/sapi4/src/*.c))
SAPI4NFLAGS = -std=c11 -ffp-contract=off -fno-builtin -w -IEngine/sapi4/src -IEngine/sapi4/port -IEngine/sapi4/lib
SAPI4LIB = SAPI4/emu/x86.c SAPI4/emu/emu.c SAPI4/emu/crt.c SAPI4/emu/winapi.c SAPI4/emu/ole.c \
           SAPI4/emu/registry.c SAPI4/emu/sapi4_tts.c
SWIFT_SSML = Shared/SSML.swift Shared/SSMLCapture.swift Shared/SingingMode.swift Shared/SpeechRate.swift Shared/Voices.swift \
             Shared/ClassicEngine.swift Tests/ssml/main.swift
SSMLOBJ = build/mac/ssml

build/mac/ssml_test: $(SWIFT_SSML) $(ENGINE) $(BRIDGE) Shared/Bridge/cv4n_bridge.c Shared/Bridge/cv_resample.c \
                     Shared/Bridge/cva_bridge.c Shared/Bridge/cvo_bridge.c Shared/Bridge/cvt_bridge.c Shared/Bridge/cvn_bridge.c \
                     $(SAPI4N) $(ANNA) $(ONECORE) \
                     $(OPENTV) $(TVDATA) Shared/Bridge/*.h Engine/anna/*.h Engine/onecore/*.h Engine/opentv/include/tvtts.h \
                     Engine/sapi4/src/*.h Engine/sapi4/lib/sapi4tts.h
	mkdir -p $(SSMLOBJ)
	rm -f $(SSMLOBJ)/*.o
	cd $(SSMLOBJ) && clang -c -O2 -std=c99 -w -DCV_NO_DEBUG_ENV -I../../../Engine/sam -I../../../Shared/Bridge \
	  $(addprefix ../../../,$(ENGINE) $(BRIDGE))
	cd $(SSMLOBJ) && clang -c -O2 -w -DCV_NO_DEBUG_ENV -I../../../Engine/sam -I../../../Shared/Bridge -I../../../Engine/sapi4/lib \
	  -I../../../Engine/anna -I../../../Engine/onecore ../../../Shared/Bridge/cv4n_bridge.c ../../../Shared/Bridge/cv_resample.c \
	  ../../../Shared/Bridge/cva_bridge.c ../../../Shared/Bridge/cvo_bridge.c ../../../Shared/Bridge/cvn_bridge.c
	cd $(SSMLOBJ) && clang -c -O2 -std=c99 -w -DCV_NO_DEBUG_ENV -I../../../Engine/sam -I../../../Shared/Bridge \
	  -I../../../Engine/opentv/include ../../../Shared/Bridge/cvt_bridge.c
	cd $(SSMLOBJ) && clang -c -O2 $(OPENTVFLAGS) $(addprefix -I../../../,Engine/opentv/src Engine/opentv/include \
	  Engine/opentv/generated) $(addprefix ../../../,$(OPENTV)) ../../../$(TVDATA)
	cd $(SSMLOBJ) && clang -c -O2 -std=c99 -ffp-contract=off -w -DCV_NO_DEBUG_ENV $(addprefix ../../../,$(ANNA))
	cd $(SSMLOBJ) && clang -c -O2 -std=c99 -ffp-contract=off -fno-fast-math -w -DCV_NO_DEBUG_ENV $(addprefix ../../../,$(ONECORE))
	mkdir -p $(SSMLOBJ)/sapi4 && rm -f $(SSMLOBJ)/sapi4/*.o   # (its own folder: lib/stubs.c would overwrite OpenTV's stubs.o)
	cd $(SSMLOBJ)/sapi4 && clang -c -O2 $(SAPI4NFLAGS) $(addprefix -I../../../../,Engine/sapi4/src Engine/sapi4/port Engine/sapi4/lib) \
	  $(addprefix ../../../../,$(SAPI4N))
	swiftc -O -import-objc-header Shared/Bridge/ClassicVoices-Bridging-Header.h \
	  -Xcc -IShared/Bridge -Xcc -IEngine/sam -Xcc -IEngine/sapi4/lib -Xcc -IEngine/anna -Xcc -IEngine/onecore \
	  -Xcc -IEngine/opentv/include $(SWIFT_SSML) \
	  $(SSMLOBJ)/*.o $(SSMLOBJ)/sapi4/*.o -o $@

ssml-test: build/mac/ssml_test
	./build/mac/ssml_test parse
	CV_DATA_ROOT=$(CURDIR) ./build/mac/ssml_test render samples/ssml
	CV_DATA_ROOT=$(CURDIR) ./build/mac/ssml_test singscores samples/ssml-sing
	CV_DATA_ROOT=$(CURDIR) ./build/mac/ssml_test neural samples/ssml-neural

.PHONY: ssml-test

# The app's Play / Stop button (App/PreviewPlayer.swift): play, stop at once, stop while synthesizing, stale audio
# ignored, reset to Play when the clip ends. Plays silence at volume 0.
build/mac/preview_test: App/PreviewPlayer.swift Tests/preview/main.swift
	mkdir -p build/mac
	swiftc -O App/PreviewPlayer.swift Tests/preview/main.swift -o $@

preview-test: build/mac/preview_test
	./build/mac/preview_test

.PHONY: preview-test

# Microsoft Anna (Engine/anna, the ms-ana-decomp reconstruction of the Vista / 7 TTS20 engine) and its bridge, exactly
# as the iOS targets compile them. The engine must be built WITHOUT floating-point contraction (-ffp-contract=off): its
# author verified it bit-exact with the original only that way, and its decoder refuses to open otherwise.
#   make anna-data    hash-check AnnaVoice/ (tools/anna_data.sha256) + whole-voice integrity (tools/anna_integrity.py)
#   make anna-test    engine + bridge under ASan/UBSan: sentences, hostile text, rate/pitch, trim, cancel, state, threads
#   make anna-tsan    two voices on two threads under ThreadSanitizer
#   make anna-bench   open time, first-audio latency, real-time factor, memory per open voice (plain -O2 build)
ANNABRIDGE = Shared/Bridge/cva_bridge.c Shared/Bridge/cv_resample.c Shared/Bridge/cv_bridge.c
ANNAFLAGS = -std=c99 -ffp-contract=off -Wall -Wno-unused-parameter -Wno-unused-function -DCV_NO_DEBUG_ENV \
            -IEngine/anna -IEngine/sam -IShared/Bridge
ANNADATA ?= AnnaVoice
ANNADEPS = $(ANNA) $(ANNABRIDGE) $(ENGINE) Engine/anna/*.h Shared/Bridge/*.h Engine/test/anna_test.c

build/mac/anna_test_asan: $(ANNADEPS)
	mkdir -p build/mac
	$(CC) $(ANNAFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(ANNA) $(ANNABRIDGE) $(ENGINE) \
	  Engine/test/anna_test.c -lm -lpthread -o $@

build/mac/anna_test_tsan: $(ANNADEPS)
	mkdir -p build/mac
	$(CC) $(ANNAFLAGS) -O1 -g -fsanitize=thread $(ANNA) $(ANNABRIDGE) $(ENGINE) Engine/test/anna_test.c -lm -lpthread -o $@

build/mac/anna_test: $(ANNADEPS)
	mkdir -p build/mac
	$(CC) $(ANNAFLAGS) -O2 $(ANNA) $(ANNABRIDGE) $(ENGINE) Engine/test/anna_test.c -lm -lpthread -o $@

anna-data:
	cd $(ANNADATA) && shasum -a 256 -c ../tools/anna_data.sha256
	python3 tools/anna_integrity.py $(ANNADATA)

anna-test: build/mac/anna_test_asan
	mkdir -p samples/anna
	./build/mac/anna_test_asan $(ANNADATA) selftest samples/anna

anna-tsan: build/mac/anna_test_tsan
	./build/mac/anna_test_tsan $(ANNADATA) threads 3

anna-bench: build/mac/anna_test
	./build/mac/anna_test $(ANNADATA) bench 4

.PHONY: anna-data anna-test anna-tsan anna-bench

# Microsoft David, Zira and Mark (en-US), Hazel, George and Susan (en-GB) and the neural Eva (en-US) and Sarah (en-GB) (Engine/onecore, the ms-david-zira-decomp
# reconstruction of the Windows 10 / 11 OneCore engine, with our locale patch) and their bridge, exactly as the iOS targets compile them. Like Anna: NEVER contract floating point
# (-ffp-contract=off, no fast-math) - its author verified it bit-exact with the original only that way.
#   make onecore-data    hash-check OneCoreVoice/ (tools/onecore_data.sha256) + structural checks (tools/onecore_integrity.py)
#   make onecore-test    engine + bridge under ASan/UBSan, all eight voices: locale / phone-set / British-pronunciation checks,
#                        sentences, emotions, hostile text, rate/pitch, trim, cancel, state, 60 utterances, threads;
#                        WAVs in samples/onecore-test
#   make onecore-tsan    two voices on two threads under ThreadSanitizer (eight pairs, both locales, Eva and Sarah included)
#   make onecore-bench   open time, first-audio latency, real-time factor, memory per open voice (plain -O2 build)
ONECOREBRIDGE = Shared/Bridge/cvo_bridge.c Shared/Bridge/cv_resample.c Shared/Bridge/cv_bridge.c
ONECOREFLAGS = -std=c99 -ffp-contract=off -fno-fast-math -Wall -Wno-unused-parameter -Wno-unused-function \
               -Wno-unused-variable -Wno-unused-but-set-variable -Wno-sign-compare -DCV_NO_DEBUG_ENV \
               -IEngine/onecore -IEngine/sam -IShared/Bridge
ONECOREDATA ?= OneCoreVoice
ONECOREDEPS = $(ONECORE) $(ONECOREBRIDGE) $(ENGINE) Engine/onecore/*.h Shared/Bridge/*.h Engine/test/onecore_test.c

build/mac/onecore_test_asan: $(ONECOREDEPS)
	mkdir -p build/mac
	$(CC) $(ONECOREFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(ONECORE) $(ONECOREBRIDGE) \
	  $(ENGINE) Engine/test/onecore_test.c -lm -lpthread -o $@

build/mac/onecore_test_tsan: $(ONECOREDEPS)
	mkdir -p build/mac
	$(CC) $(ONECOREFLAGS) -O1 -g -fsanitize=thread $(ONECORE) $(ONECOREBRIDGE) $(ENGINE) Engine/test/onecore_test.c \
	  -lm -lpthread -o $@

build/mac/onecore_test: $(ONECOREDEPS)
	mkdir -p build/mac
	$(CC) $(ONECOREFLAGS) -O2 $(ONECORE) $(ONECOREBRIDGE) $(ENGINE) Engine/test/onecore_test.c -lm -lpthread -o $@

onecore-data:
	cd $(ONECOREDATA) && shasum -a 256 -c ../tools/onecore_data.sha256
	python3 tools/onecore_integrity.py $(ONECOREDATA)

onecore-test: build/mac/onecore_test_asan build/mac/onecore_test
	mkdir -p samples/onecore-test
	./build/mac/onecore_test_asan $(ONECOREDATA) selftest samples/onecore-test
	./build/mac/onecore_test $(ONECOREDATA) memory

onecore-tsan: build/mac/onecore_test_tsan
	./build/mac/onecore_test_tsan $(ONECOREDATA) threads 3

onecore-bench: build/mac/onecore_test
	./build/mac/onecore_test $(ONECOREDATA) bench 3

.PHONY: onecore-data onecore-test onecore-tsan onecore-bench

# L&H TruVoice on OpenTV (Engine/opentv, github.com/RetroBunn/tv-decomp: Centigram's TruVoice engine decompiled to C) and
# its bridge, exactly as the iOS targets compile them. The engine's tables are voice data: TruVoiceData/tvdata.s,
# generated from the pinned upstream's data/en/engine.tvdata (gitignored; see NOTICE).
#   make truvoice-data   generate + hash-check TruVoiceData/tvdata.s (needs Engine/opentv-upstream at the pinned commit)
#   make truvoice-test   engine + bridge under ASan/UBSan: every voice, hostile text, trim, rate/pitch, cancel, voices
#                        not disturbing each other, memory, threads, determinism; WAVs in samples/truvoice
#   make truvoice-tsan   two voices on two threads under ThreadSanitizer
#   make truvoice-bench  open time, first audio, real-time factor, memory per voice (plain -O2)
#   make truvoice-ab     the old path (tv_enua.dll in the SAPI 4 interpreter, SAPI4/data/tv_enua) against the new one,
#                        through both bridges; before/after WAVs in samples/truvoice-ab
TVBRIDGE = Shared/Bridge/cvt_bridge.c Shared/Bridge/cv_resample.c Shared/Bridge/cv_bridge.c
TVCFLAGS = -std=c99 -Wall -Wextra -Wno-unused-parameter -DCV_NO_DEBUG_ENV -IEngine/opentv/include -IShared/Bridge -IEngine/sam
TVDEPS = $(OPENTV) $(TVDATA) $(TVBRIDGE) $(ENGINE) Engine/opentv/src/*.h Engine/opentv/include/tvtts.h Shared/Bridge/*.h \
         Engine/test/truvoice_test.c
# The engine indexes past a table's declared size into the next table on purpose (e.g. g_phone_attr[715] of 640 in
# stage0.c): the data image keeps the original's layout, so that lands where it did in the DLL. UBSan's array-bounds
# check would report each one, so it is off for the engine's own objects (the bridge keeps it; ASan does not see the
# image, which is assembly).
TVNOBOUNDS = $(if $(findstring undefined,$(1)),-fno-sanitize=array-bounds,)
# build/mac/<name>: $(1) = extra flags (sanitizer / optimisation), $(2) = extra C sources, $(3) = extra -D / -I
define tv_build
	mkdir -p $@.o.d && rm -f $@.o.d/*.o
	cd $@.o.d && $(CC) -c $(1) $(TVNOBOUNDS) $(OPENTVFLAGS) $(addprefix -I../../../,Engine/opentv/src Engine/opentv/include Engine/opentv/generated) \
	  $(addprefix ../../../,$(OPENTV))
	cd $@.o.d && $(CC) -c ../../../$(TVDATA)
	$(CC) $(1) $(TVCFLAGS) $(3) $(TVBRIDGE) $(ENGINE) $(2) Engine/test/truvoice_test.c $@.o.d/*.o -lm -lpthread -o $@
endef

truvoice-data:
	sh tools/truvoice_data.sh

build/mac/truvoice_test_asan: $(TVDEPS)
	$(call tv_build,-O1 -g -fsanitize=address$(,)undefined -fno-omit-frame-pointer,,)

build/mac/truvoice_test_tsan: $(TVDEPS)
	$(call tv_build,-O1 -g -fsanitize=thread,,)

build/mac/truvoice_test: $(TVDEPS)
	$(call tv_build,-O2,,)

build/mac/truvoice_ab: $(TVDEPS) $(SAPI4LIB) Shared/Bridge/cv4_bridge.c
	$(call tv_build,-O2,Shared/Bridge/cv4_bridge.c $(SAPI4LIB),-DCV_TRUVOICE_AB -ISAPI4/emu)

truvoice-test: build/mac/truvoice_test_asan build/mac/truvoice_test
	mkdir -p samples/truvoice
	./build/mac/truvoice_test_asan selftest samples/truvoice
	./build/mac/truvoice_test memory

truvoice-tsan: build/mac/truvoice_test_tsan
	./build/mac/truvoice_test_tsan threads 3

truvoice-bench: build/mac/truvoice_test
	for m in "Adult Male #1" "Adult Male #2" "Adult Male #6" "Adult Female #1" "Adult Female #2"; do \
	  ./build/mac/truvoice_test bench "$$m, American English (TruVoice)" 2; done

truvoice-ab: build/mac/truvoice_ab
	mkdir -p samples/truvoice-ab
	./build/mac/truvoice_ab ab SAPI4/data/tv_enua samples/truvoice-ab

.PHONY: truvoice-data truvoice-test truvoice-tsan truvoice-bench truvoice-ab

# Microsoft's SAPI 4 engine as native C (Engine/sapi4: the sapi4-decomp library, msttssyn.dll decompiled; pinned in
# NOTICE) and its bridge (Shared/Bridge/cv4n_bridge.c), exactly as the iOS targets compile them. The engine reads the
# DLL's data sections from SAPI4Voices/msttssyn.dll at run time (nothing of Microsoft's is compiled in) and must be built
# as upstream verified it: -ffp-contract=off (no fused multiply-add: the original's x87 arithmetic rounds each step)
# and -fno-builtin.
#   make sapi4-test    engine + bridge under ASan/UBSan: every mode, hostile text, trim, rate/pitch, cancel, parking and
#                      the engine cap, voices not disturbing each other, threads, determinism; WAVs in samples/sapi4;
#                      then the memory check in the plain build
#   make sapi4-tsan    two voices on two threads under ThreadSanitizer
#   make sapi4-bench   open time, first audio, real-time factor, memory, per voice (plain -O2), and the same through the
#                      interpreter (the old path) for comparison
#   make sapi4-ab      the old path (cv4_bridge + SAPI4/emu) against the new one: the same 3,000-odd requests through both,
#                      compared PCM for PCM (Engine/test/sapi4_ab.c); WAVs of any difference in samples/sapi4-ab/diff
S4BRIDGE = Shared/Bridge/cv4n_bridge.c Shared/Bridge/cv_resample.c Shared/Bridge/cv_bridge.c
S4CFLAGS = -std=c99 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -DCV_NO_DEBUG_ENV \
           -IEngine/sapi4/lib -IShared/Bridge -IEngine/sam
S4DATA ?= SAPI4Voices
S4DEPS = $(SAPI4N) $(S4BRIDGE) $(ENGINE) Engine/sapi4/src/*.h Engine/sapi4/lib/sapi4tts.h Shared/Bridge/*.h
# The decompiled engine keeps the original's 32-bit structure layouts, so some pointers and 16-bit fields sit at 4- and
# 1-byte offsets (fe_vm.c's rule headers, senone.c, vload.c). arm64 and x86_64 load and store those without alignment;
# UBSan's alignment check would report every one, so it is off for the engine's own objects (the bridge keeps it).
S4NOALIGN = $(if $(findstring undefined,$(1)),-fno-sanitize=alignment,)
# build/mac/<name>: $(1) = extra flags (sanitizer / optimisation), $(2) = the driver
define s4_build
	mkdir -p $@.o.d && rm -f $@.o.d/*.o
	cd $@.o.d && $(CC) -c -O2 $(1) $(S4NOALIGN) $(SAPI4NFLAGS) $(addprefix -I../../../,Engine/sapi4/src Engine/sapi4/port Engine/sapi4/lib) \
	  $(addprefix ../../../,$(SAPI4N))
	$(CC) $(1) $(S4CFLAGS) $(S4BRIDGE) $(ENGINE) $(2) $@.o.d/*.o -lm -lpthread -o $@
endef

build/mac/sapi4_test_asan: $(S4DEPS) Engine/test/sapi4_test.c
	$(call s4_build,-O1 -g -fsanitize=address$(,)undefined -fno-omit-frame-pointer,Engine/test/sapi4_test.c)

build/mac/sapi4_test_tsan: $(S4DEPS) Engine/test/sapi4_test.c
	$(call s4_build,-O1 -g -fsanitize=thread,Engine/test/sapi4_test.c)

build/mac/sapi4_test: $(S4DEPS) Engine/test/sapi4_test.c
	$(call s4_build,-O2,Engine/test/sapi4_test.c)

build/mac/sapi4_ab_new: $(S4DEPS) Engine/test/sapi4_ab.c
	$(call s4_build,-O2,Engine/test/sapi4_ab.c)

# the old path, compiled as the app compiled it until now (SAPI4/emu at -O3 -std=gnu11)
build/mac/sapi4_ab_old: $(SAPI4LIB) SAPI4/emu/*.h Shared/Bridge/cv4_bridge.c Shared/Bridge/cv_resample.c Shared/Bridge/cv_bridge.c \
                        $(ENGINE) Engine/test/sapi4_ab.c
	mkdir -p $@.o.d && rm -f $@.o.d/*.o
	cd $@.o.d && $(CC) -c -O3 -std=gnu11 -w -DCV_NO_DEBUG_ENV -I../../../SAPI4/emu $(addprefix ../../../,$(SAPI4LIB))
	$(CC) -O2 -std=c99 -w -DCV_NO_DEBUG_ENV -DCV_SAPI4_OLD -ISAPI4/emu -IShared/Bridge -IEngine/sam Shared/Bridge/cv4_bridge.c \
	  Shared/Bridge/cv_resample.c Shared/Bridge/cv_bridge.c $(ENGINE) Engine/test/sapi4_ab.c $@.o.d/*.o -lm -lpthread -o $@

sapi4-test: build/mac/sapi4_test_asan build/mac/sapi4_test
	mkdir -p samples/sapi4
	./build/mac/sapi4_test_asan $(S4DATA) selftest samples/sapi4
	./build/mac/sapi4_test $(S4DATA) memory

sapi4-tsan: build/mac/sapi4_test_tsan
	./build/mac/sapi4_test_tsan $(S4DATA) threads 3

sapi4-bench: build/mac/sapi4_test SAPI4/build/cv4_test
	for m in "Sam" "Mike" "Mary" "Mike (for Telephone)" "Mary in Hall" "Male Whisper" "RoboSoft One"; do \
	  ./build/mac/sapi4_test $(S4DATA) bench "$$m" 2; done
	@echo "the old path (msttssyn.dll in the interpreter), same sentence, warm:"
	for m in "Sam" "Mary in Hall" "Mike (for Telephone)"; do ./SAPI4/build/cv4_test $(S4DATA) bench "$$m" 2; done

SAPI4/build/cv4_test:
	$(MAKE) -C SAPI4 build/cv4_test

sapi4-ab: build/mac/sapi4_ab_old build/mac/sapi4_ab_new
	rm -rf samples/sapi4-ab && mkdir -p samples/sapi4-ab
	./build/mac/sapi4_ab_old render $(S4DATA) samples/sapi4-ab/old
	./build/mac/sapi4_ab_new render $(S4DATA) samples/sapi4-ab/new
	./build/mac/sapi4_ab_new compare samples/sapi4-ab/old samples/sapi4-ab/new samples/sapi4-ab/diff

.PHONY: sapi4-test sapi4-tsan sapi4-bench sapi4-ab

# The neural voices - Jenny, Aria, Guy (en-US), Sonia, Ryan (en-GB), the Windows 11 natural voices on Microsoft's embedded
# Speech SDK (NeuralSDK/macos: the iOS dylibs re-tagged for macOS, the same arm64 code) - through cvn_bridge.c exactly as the
# iOS targets compile it (Engine/test/neural_test.c).
#   make neural-stage    stage NeuralVoices/ + NeuralSDK/ from ~/code/NeuralVoice (tools/neural_stage.py: Appx signatures, block
#                        maps, the files shared with OneCoreVoice) and compare with tools/neural_data.sha256
#   make neural-data     hash-check what is staged (the build needs NeuralVoices/ and NeuralSDK/)
#   make neural-test     bridge under UBSan with macOS's malloc guards (NOT ASan: under ASan the SDK's
#                        synthesizer_create_speech_synthesizer_from_config fails, measured 2026-09-27): every voice,
#                        determinism, rate / pitch, hostile text, cancel, streaming; then the memory check in the plain
#                        build; WAVs in samples/neural
#   make neural-threads  two voices on two threads, each identical to alone (plain build: under ThreadSanitizer, as under
#                        ASan, the SDK refuses to create a synthesizer; the bridge serialises everything under one mutex)
#   make neural-bench    open, first audio, real-time factor, memory per voice (plain -O2 build)
NEURALBRIDGE = Shared/Bridge/cvn_bridge.c Shared/Bridge/cv_resample.c Shared/Bridge/cv_bridge.c
NEURALFLAGS = -std=c99 -Wall -Wno-unused-parameter -DCV_NO_DEBUG_ENV -IShared/Bridge -IEngine/sam
NEURALDEPS = $(NEURALBRIDGE) $(ENGINE) Shared/Bridge/*.h Engine/test/neural_test.c

build/mac/neural_test_ubsan: $(NEURALDEPS)
	mkdir -p build/mac
	$(CC) $(NEURALFLAGS) -O1 -g -fsanitize=undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer $(NEURALBRIDGE) \
	  $(ENGINE) Engine/test/neural_test.c -lm -lpthread -o $@

build/mac/neural_test: $(NEURALDEPS)
	mkdir -p build/mac
	$(CC) $(NEURALFLAGS) -O2 $(NEURALBRIDGE) $(ENGINE) Engine/test/neural_test.c -lm -lpthread -o $@

neural-stage:
	python3 tools/neural_stage.py

neural-data:
	shasum -a 256 -c tools/neural_data.sha256 | grep -v ': OK$$' || true
	shasum -a 256 -c --quiet tools/neural_data.sha256

neural-test: build/mac/neural_test_ubsan build/mac/neural_test
	mkdir -p samples/neural
	MallocScribble=1 MallocPreScribble=1 MallocGuardEdges=1 MallocCheckHeapStart=1000 MallocCheckHeapEach=1000 \
	  ./build/mac/neural_test_ubsan $(CURDIR) selftest samples/neural
	mkdir -p build/neural-plain && ./build/mac/neural_test $(CURDIR) selftest build/neural-plain

neural-threads: build/mac/neural_test
	./build/mac/neural_test $(CURDIR) threads 5

neural-bench: build/mac/neural_test
	./build/mac/neural_test $(CURDIR) bench

.PHONY: neural-stage neural-data neural-test neural-threads neural-bench

# A release: builds the IPA and the notarized Mac zip, publishes them with the AltStore source and the downloads page
# (tools/release.sh; settings in tools/release.local).
#   make release VERSION=1.1 NOTES="What's new"            DRYRUN=1 builds and shows the changes without publishing
release:
	@test -n "$(VERSION)" -a -n "$(NOTES)" || { echo 'usage: make release VERSION=1.1 NOTES="What'"'"'s new" [DRYRUN=1]'; exit 2; }
	tools/release.sh "$(VERSION)" "$(NOTES)" $(if $(DRYRUN),--dry-run)

.PHONY: release
