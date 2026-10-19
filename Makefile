CC     ?= cc
CFLAGS ?= -O2 -g
STD    := -std=gnu11
WARN   := -Wall -Wextra -Wshadow

all: heg

heg: heg.c
	$(CC) $(STD) $(CFLAGS) $(WARN) -o $@ $<

# build with AddressSanitizer + UBSan to catch memory bugs
heg-asan: heg.c
	$(CC) $(STD) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(WARN) -o $@ $<

test: heg
	./test_shell.sh ./heg
	python3 test_jobs.py ./heg
	python3 test_editor.py ./heg

test-asan: heg-asan
	./test_shell.sh ./heg-asan
	python3 test_jobs.py ./heg-asan
	python3 test_editor.py ./heg-asan

# hegterm: Matrix-style terminal window for heg (needs: sudo apt install libsdl2-dev libsdl2-ttf-dev)
SDL_FLAGS = $(shell sdl2-config --cflags --libs 2>/dev/null) -lSDL2_ttf -lutil -lm

hegterm: term/term.c term/term.h term/hegterm.c
	$(CC) -std=gnu11 $(CFLAGS) $(WARN) -o $@ term/term.c term/hegterm.c $(SDL_FLAGS)

# the screen model and an end-to-end heg-in-a-pty test (no SDL or display needed)
test-term: heg term/term.c term/test_term.c term/term.h
	$(CC) -std=gnu11 -O1 -g -fsanitize=address,undefined $(WARN) -o term/test_term term/term.c term/test_term.c -lutil
	./term/test_term ./heg

# heg-lite: the freestanding, kernel-portable core (see KERNEL_PORT.md)
hegl: kernel/hegl.c kernel/hegl_sys.h
	$(CC) -std=gnu99 $(CFLAGS) $(WARN) -o kernel/hegl kernel/hegl.c

test-lite: hegl
	./kernel/test_hegl.sh $(CURDIR)/kernel/hegl

dist: clean
	rm -f heg.zip
	zip -r heg.zip heg.c Makefile README.md .gitignore test_shell.sh test_jobs.py test_editor.py kernel/hegl.c kernel/hegl_sys.h kernel/test_hegl.sh kernel/KERNEL_PORT.md term/term.c term/term.h term/hegterm.c term/test_term.c
	@echo built heg.zip

clean:
	rm -f heg heg-asan hegterm kernel/hegl term/test_term heg.zip


.PHONY: all test test-asan test-lite hegl hegterm test-term dist clean
