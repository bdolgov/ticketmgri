MGR = billmgr
PLUGIN = ticketmgri
VERSION = 5.0.1
LIB += ticketmgri
ticketmgri_SOURCES = ticketmgri.cpp

WRAPPER += ticketmgri_syncticket
ticketmgri_syncticket_SOURCES = ticketmgri_syncticket.cpp
ticketmgri_syncticket_LDADD = -lbase -lprocessingmodule

BASE ?= /usr/local/mgr5
include $(BASE)/src/isp.mk
