TOPDIR=../
include $(TOPDIR)Rules.make

ifeq ($(LINUX),1)
LINUX_OBJ=poutput-vcsa.o poutput-fb.o
LINUX_LIBS=$(ICONV_LIBS)
endif

hardware_so=hardware.o freq.o
sets_so=sets.o
poutput_so=console.o cp437.o latin1.o poutput-curses.o $(LINUX_OBJ) poutput-keyboard.o pfonts.o poutput.o utf-8.o
poutput_so_libs=$(ICONV_LIBS)
ifeq ($(HAVE_X11),1)
poutput_so+=poutput-x11.o x11-common.o
poutput_so_libs+=$(X11_LIBS)
NEED_TTF=1
endif
ifeq ($(HAVE_SDL),1)
poutput_so+=poutput-sdl.o
poutput_so_libs+=$(SDL_LIBS)
NEED_TTF=1
endif
ifeq ($(HAVE_SDL2),1)
poutput_so+=poutput-sdl2.o
poutput_so_libs+=$(SDL2_LIBS)
NEED_TTF=1
endif

ifeq ($(NEED_TTF),1)
poutput_so+=ttf.o poutput-swtext.o poutput-fontengine.o
poutput_so_libs+=$(FREETYPE2_LIBS)
endif

ifeq ($(STATIC_BUILD),1)
all: compat.o err.o framelock.o irq.o timer.o $(hardware_so) $(sets_so) $(poutput_so)
else
all: compat.o err.o framelock.o irq.o timer.o hardware$(LIB_SUFFIX) sets$(LIB_SUFFIX) poutput$(LIB_SUFFIX)
endif

test: compat-test
	./compat-test.sh

poutput$(LIB_SUFFIX): $(poutput_so)
	$(CC) $(SHARED_FLAGS) $(LINUX_LIBS) -o $@ $^ $(NCURSES_LIBS) $(poutput_so_libs)

hardware$(LIB_SUFFIX): $(hardware_so)
	$(CC) $(SHARED_FLAGS) -o $@ $^

sets$(LIB_SUFFIX): $(sets_so)
	$(CC) $(SHARED_FLAGS) -o $@ $^

clean:
	rm -f *.o *$(LIB_SUFFIX) compat-test

ifeq ($(STATIC_BUILD),1)
install:
uninstall:
else
install:
	$(CP) poutput$(LIB_SUFFIX) "$(DESTDIR)$(LIBDIR)/autoload/12-poutput$(LIB_SUFFIX)"
	$(CP) sets$(LIB_SUFFIX) "$(DESTDIR)$(LIBDIR)/autoload/10-sets$(LIB_SUFFIX)"
	$(CP) hardware$(LIB_SUFFIX) "$(DESTDIR)$(LIBDIR)/autoload/10-hardware$(LIB_SUFFIX)"

uninstall:
	rm -f "$(DESTDIR)$(LIBDIR)/hardware$(LIB_SUFFIX)" "$(DESTDIR)$(LIBDIR)/autoload/12-poutput$(LIB_SUFFIX)" "$(DESTDIR)$(LIBDIR)/autoload/10-sets$(LIB_SUFFIX)" "$(DESTDIR)$(LIBDIR)/autoload/10-hardware$(LIB_SUFFIX)"
endif

cp437.o: cp437.c cp437.h \
	../config.h \
	../types.h \
	utf-8.h
	$(CC) $< -o $@ -c

compat-test: compat-test.c \
	compat.c \
	compat.h \
	../config.h \
	../types.h \
	../boot/plinkman.h
	$(CC) $< -o $@

poutput.o: poutput.h poutput.c \
	../config.h \
	../types.h \
	imsrtns.h \
	pfonts.h
	$(CC) poutput.c -o $@ -c

poutput-keyboard.o: poutput-keyboard.c \
	poutput.h \
	../config.h \
	../types.h
	$(CC) poutput-keyboard.c -o $@ -c

timer.o: timer.c timer.h irq.h \
	../config.h \
	../types.h \
	compat.h \
	imsrtns.h \
	poll.h
	$(CC) timer.c -o $@ -c

irq.o: irq.c irq.h \
	../config.h \
	../types.h
	$(CC) irq.c -o $@ -c

latin1.o: latin1.c latin1.h \
	../config.h \
	../types.h \
	utf-8.h
	$(CC) $< -o $@ -c

poutput-curses.o: poutput-curses.c poutput-curses.h \
	../config.h \
	../types.h \
	poutput.h \
	../boot/console.h \
	../boot/psetting.h \
	cp437.h \
	utf-8.h
	$(CC) poutput-curses.c -o $@ -c

poutput-vcsa.o: poutput-vcsa.c poutput-vcsa.h \
	../config.h \
	../types.h \
	poutput.h \
	../boot/console.h \
	../boot/psetting.h \
	pfonts.h \
	utf-8.h
	$(CC) poutput-vcsa.c -o $@ -c

poutput-fb.o: poutput-fb.c poutput-fb.h \
	../config.h \
	../types.h \
	../boot/console.h \
	poutput.h
	$(CC) poutput-fb.c -o $@ -c

poutput-fontengine.o: poutput-fontengine.c poutput-fontengine.h \
	../config.h \
	../types.h \
	pfonts.h \
	cp437.h \
	ttf.h \
	utf-8.h
	$(CC) $< -o $@ -c

poutput-sdl.o: poutput-sdl.c poutput-sdl.h \
	ttf.h \
	../config.h \
	../types.h \
	../boot/console.h \
	../boot/psetting.h \
	framelock.h \
	poutput.h \
	poutput-fontengine.h \
	poutput-swtext.h
	$(CC) $(SDL_CFLAGS) poutput-sdl.c -o $@ -c

poutput-sdl2.o: poutput-sdl2.c poutput-sdl2.h \
	ttf.h \
	../config.h \
	../types.h \
	../boot/console.h \
	../boot/psetting.h \
	framelock.h \
	poutput.h \
	poutput-fontengine.h \
	poutput-swtext.h
	$(CC) $(SDL2_CFLAGS) poutput-sdl2.c -o $@ -c

poutput-swtext.o: poutput-swtext.c poutput-swtext.h \
	framelock.h \
	latin1.h \
	pfonts.h \
	poutput.h \
	poutput-fontengine.h \
	utf-8.h \
	../config.h \
	../types.h
	$(CC) $(CFLAGS) $< -o $@ -c

poutput-x11.o: poutput-x11.c poutput-x11.h \
	../config.h \
	../types.h \
	../boot/console.h \
	../boot/psetting.h \
	../desktop/opencubicplayer-48x48.xpm \
	framelock.h \
	poutput.h \
	poutput-swtext.h \
	pfonts.h \
	x11-common.h
	$(CC) poutput-x11.c -o $@ -c

x11-common.o: x11-common.c x11-common.h \
	../config.h \
	../types.h \
	poutput.h
	$(CC) x11-common.c -o $@ -c

freq.o: freq.c freq.h imsrtns.h \
	../config.h \
	../types.h
	$(CC) freq.c -o $@ -c

err.o: err.c err.h \
	../config.h \
	../types.h
	$(CC) err.c -o $@ -c

sets.o: sets.c sets.h \
	../config.h \
	../types.h \
	../boot/plinkman.h \
	../boot/psetting.h \
	err.h
	$(CC) sets.c -o $@ -c

hardware.o: hardware.c \
	../config.h \
	../types.h \
	../boot/plinkman.h
	$(CC) hardware.c -o $@ -c

console.o: console.c \
	../boot/console.h \
	../config.h \
	../boot/psetting.h \
	../stuff/latin1.h \
	poutput-curses.h \
	poutput-fb.h \
	poutput-sdl.h \
	poutput-sdl2.h \
	poutput-x11.h \
	poutput-vcsa.h \
	utf-8.h
	$(CC) console.c -o $@ -c

compat.o: compat.c \
	../config.h \
	../types.h \
	../boot/plinkman.h \
	compat.h
	$(CC) compat.c -o $@ -c

pfonts.o: pfonts.c \
	../config.h \
	../types.h \
	pfonts.h
	$(CC) pfonts.c -o $@ -c

framelock.o: framelock.c framelock.h \
	../config.h \
	../types.h \
	../boot/plinkman.h \
	../boot/psetting.h \
	../filesel/pfilesel.h \
	timer.h \
	err.h
	$(CC) framelock.c -o $@ -c

ttf.o: ttf.c ttf.h \
	utf-8.h
	$(CC) $< -o $@ -c $(FREETYPE2_CFLAGS) $(FREETYPE2_CFLAGS)

utf-8.o: utf-8.c \
	../config.h \
	../types.h \
	../cpiface/cpiface.h \
	poutput.h \
	framelock.h \
	utf-8.h
	$(CC) $< -o $@ -c
