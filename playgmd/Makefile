TOPDIR=../
include $(TOPDIR)Rules.make

ifeq ($(BUILD_DUMPTOOLS),1)
	DUMPERS=dumpstm dumps3m
else
	DUMPERS=
endif

all: gmdptype.o load669$(LIB_SUFFIX) loadams$(LIB_SUFFIX) loaddmf$(LIB_SUFFIX) loadmdl$(LIB_SUFFIX) loadmtm$(LIB_SUFFIX) loadokt$(LIB_SUFFIX) loadptm$(LIB_SUFFIX) loads3m$(LIB_SUFFIX) loadstm$(LIB_SUFFIX) loadult$(LIB_SUFFIX) playgmd$(LIB_SUFFIX) $(DUMPERS)

playgmd_so=gmdpchan.o gmdpdots.o gmdpinst.o gmdplay.o gmdpplay.o gmdptrak.o gmdrtns.o gmdtime.o
playgmd$(LIB_SUFFIX): $(playgmd_so)
	$(CC) $(SHARED_FLAGS) -o $@ $^

load669_so=gmdl669.o
load669$(LIB_SUFFIX): $(load669_so)
	$(CC) $(SHARED_FLAGS) -o $@ $^

loadams_so=gmdlams.o
loadams$(LIB_SUFFIX): $(loadams_so)
	$(CC) $(SHARED_FLAGS) -o $@ $^

loaddmf_so=gmdldmf.o
loaddmf$(LIB_SUFFIX): $(loaddmf_so)
	$(CC) $(SHARED_FLAGS) -o $@ $^

loadmdl_so=gmdlmdl.o
loadmdl$(LIB_SUFFIX): $(loadmdl_so)
	$(CC) $(SHARED_FLAGS) -o $@ $^

loadmtm_so=gmdlmtm.o
loadmtm$(LIB_SUFFIX): $(loadmtm_so)
	$(CC) $(SHARED_FLAGS) -o $@ $^

loadokt_so=gmdlokt.o
loadokt$(LIB_SUFFIX): $(loadokt_so)
	$(CC) $(SHARED_FLAGS) -o $@ $^

loadptm_so=gmdlptm.o
loadptm$(LIB_SUFFIX): $(loadptm_so)
	$(CC) $(SHARED_FLAGS) -o $@ $^

loads3m_so=gmdls3m.o
loads3m$(LIB_SUFFIX): $(loads3m_so)
	$(CC) $(SHARED_FLAGS) -o $@ $^

loadstm_so=gmdlstm.o
loadstm$(LIB_SUFFIX): $(loadstm_so)
	$(CC) $(SHARED_FLAGS) -o $@ $^

loadult_so=gmdlult.o
loadult$(LIB_SUFFIX): $(loadult_so)
	$(CC) $(SHARED_FLAGS) -o $@ $^

clean:
	rm -f *.o *$(LIB_SUFFIX) dumpstm dumps3m

install:
	$(CP) load669$(LIB_SUFFIX) loadams$(LIB_SUFFIX) loaddmf$(LIB_SUFFIX) loadmdl$(LIB_SUFFIX) loadmtm$(LIB_SUFFIX) loadokt$(LIB_SUFFIX) loadptm$(LIB_SUFFIX) loads3m$(LIB_SUFFIX) loadstm$(LIB_SUFFIX) loadult$(LIB_SUFFIX) playgmd$(LIB_SUFFIX) "$(DESTDIR)$(LIBDIR)"
ifeq ($(BUILD_DUMPTOOLS),1)
	$(CP) dumpstm "$(DESTDIR)$(BINDIR)/dumpstm"
	$(CP) dumps3m "$(DESTDIR)$(BINDIR)/dumps3m"
endif

uninstall:
	rm -f "$(DESTDIR)$(LIBDIR)/load669$(LIB_SUFFIX)" "$(DESTDIR)$(LIBDIR)/loadams$(LIB_SUFFIX)" "$(DESTDIR)$(LIBDIR)/loaddmf$(LIB_SUFFIX)" "$(DESTDIR)$(LIBDIR)/loadmdl$(LIB_SUFFIX)" "$(DESTDIR)$(LIBDIR)/loadmtm$(LIB_SUFFIX)" "$(DESTDIR)$(LIBDIR)/loadokt$(LIB_SUFFIX)" "$(DESTDIR)$(LIBDIR)/loadptm$(LIB_SUFFIX)" "$(DESTDIR)$(LIBDIR)/loads3m$(LIB_SUFFIX)" "$(DESTDIR)$(LIBDIR)/loadstm$(LIB_SUFFIX)" "$(DESTDIR)$(LIBDIR)/loadult$(LIB_SUFFIX)" "$(DESTDIR)$(LIBDIR)/playgmd$(LIB_SUFFIX)"
	rm -f "$(DESTDIR)$(BINDIR)/dumpstm"
	rm -f "$(DESTDIR)$(BINDIR)/dumps3m"

dumpstm: dumpstm.o
	$(CC) $(LDFLAGS) -o $@ $^

dumpstm.o: \
	dumpstm.c
	$(CC) -o $@ $< -c

dumps3m: dumps3m.o
	$(CC) $(LDFLAGS) -o $@ $^

dumps3m.o: \
	dumps3m.c
	$(CC) -o $@ $< -c

gmdptype.o: gmdptype.c \
	../config.h \
	../types.h \
	../filesel/filesystem.h \
	../filesel/mdb.h \
	../filesel/pfilesel.h \
	../stuff/cp437.h
	$(CC) gmdptype.c -o $@ -c

gmdl669.o: gmdl669.c \
	../config.h \
	../types.h \
	../boot/plinkman.h \
	../dev/mcp.h \
	../filesel/filesystem.h \
	gmdplay.h \
	gmdinst.h \
	../stuff/err.h
	$(CC) gmdl669.c -o $@ -c

gmdlams.o: gmdlams.c \
	../config.h \
	../types.h \
	../boot/plinkman.h \
	../dev/mcp.h \
	../filesel/filesystem.h \
	gmdplay.h \
	gmdinst.h \
	../stuff/err.h
	$(CC) gmdlams.c -o $@ -c

gmdldmf.o: gmdldmf.c \
	../config.h \
	../types.h \
	../boot/plinkman.h \
	../dev/mcp.h \
	../filesel/filesystem.h \
	gmdplay.h \
	gmdinst.h \
	../stuff/err.h
	$(CC) gmdldmf.c -o $@ -c

gmdlmdl.o: gmdlmdl.c \
	../config.h \
	../types.h \
	../boot/plinkman.h \
	../dev/mcp.h \
	../filesel/filesystem.h \
	gmdplay.h \
	gmdinst.h \
	../stuff/err.h
	$(CC) gmdlmdl.c -o $@ -c

gmdlmtm.o: gmdlmtm.c \
	../config.h \
	../types.h \
	../boot/plinkman.h \
	../dev/mcp.h \
	../filesel/filesystem.h \
	gmdplay.h \
	gmdinst.h \
	../stuff/err.h
	$(CC) gmdlmtm.c -o $@ -c

gmdlokt.o: gmdlokt.c \
	../config.h \
	../types.h \
	../boot/plinkman.h \
	../dev/mcp.h \
	../filesel/filesystem.h \
	gmdplay.h \
	gmdinst.h \
	../stuff/err.h
	$(CC) gmdlokt.c -o $@ -c

gmdlptm.o: gmdlptm.c \
	../config.h \
	../types.h \
	../boot/plinkman.h \
	../dev/mcp.h \
	../filesel/filesystem.h \
	gmdplay.h \
	gmdinst.h \
	../stuff/err.h
	$(CC) gmdlptm.c -o $@ -c

gmdls3m.o: gmdls3m.c \
	../config.h \
	../types.h \
	../boot/plinkman.h \
	../dev/mcp.h \
	../filesel/filesystem.h \
	gmdplay.h \
	gmdinst.h \
	../stuff/err.h
	$(CC) gmdls3m.c -o $@ -c

gmdlstm.o: gmdlstm.c \
	../config.h \
	../types.h \
	../boot/plinkman.h \
	../dev/mcp.h \
	../filesel/filesystem.h \
	gmdplay.h \
	gmdinst.h \
	../stuff/err.h
	$(CC) gmdlstm.c -o $@ -c

gmdlult.o: gmdlult.c \
	../config.h \
	../types.h \
	../boot/plinkman.h \
	../dev/mcp.h \
	../filesel/filesystem.h \
	gmdplay.h \
	gmdinst.h \
	../stuff/err.h
	$(CC) gmdlult.c -o $@ -c

gmdpchan.o: gmdpchan.c \
	../config.h \
	../types.h \
	gmdplay.h \
	gmdpchan.h \
	../stuff/poutput.h \
	../cpiface/cpiface.h
	$(CC) gmdpchan.c -o $@ -c

gmdpdots.o: gmdpdots.c \
	../config.h \
	../types.h \
	../cpiface/cpiface.h \
	../dev/mcp.h \
	gmdplay.h \
	gmdpdots.h
	$(CC) gmdpdots.c -o $@ -c

gmdpinst.o: gmdpinst.c \
	../config.h \
	../types.h \
	../dev/mcp.h \
	gmdinst.h \
	../cpiface/cpiface.h
	$(CC) gmdpinst.c -o $@ -c

gmdplay.o: gmdplay.c \
	../config.h \
	../types.h \
	../dev/mcp.h \
	gmdplay.h \
	../stuff/imsrtns.h \
	../cpiface/cpiface.h
	$(CC) gmdplay.c -o $@ -c

gmdpplay.o: gmdpplay.c \
	../config.h \
	../types.h \
	../boot/plinkman.h \
	../boot/psetting.h \
	../cpiface/cpiface.h \
	../dev/mcp.h \
	../filesel/dirdb.h \
	../filesel/filesystem.h \
	../filesel/mdb.h \
	../filesel/pfilesel.h \
	gmdpchan.h \
	gmdpdots.h \
	gmdplay.h \
	gmdptrak.h \
	../stuff/compat.h \
	../stuff/err.h \
	../stuff/poutput.h \
	../stuff/sets.h
	$(CC) gmdpplay.c -o $@ -c

gmdptrak.o: gmdptrak.c \
	../config.h \
	../types.h \
	gmdplay.h \
	gmdptrak.h \
	../stuff/poutput.h \
	../cpiface/cpiface.h
	$(CC) gmdptrak.c -o $@ -c

gmdrtns.o: gmdrtns.c \
	../config.h \
	../types.h \
	../dev/mcp.h \
	gmdplay.h
	$(CC) gmdrtns.c -o $@ -c

gmdtime.o: gmdtime.c \
	../config.h \
	../types.h \
	gmdplay.h
	$(CC) gmdtime.c -o $@ -c
