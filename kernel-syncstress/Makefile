KDIR ?= /lib/modules/$(shell uname -r)/build
BDIR ?= $(CURDIR)/build
BDIR_MAKEFILE ?= $(BDIR)/Makefile

STRESSMOD = ht
# STRESSMOD = stress

obj-m += $(STRESSMOD).o

$(STRESSMOD)-objs := rcuht.o
# $(STRESSMOD)-objs := sync-stresser.o

$(STRESSMOD)-objs += locks/qspinlock.o # Linux's qspinlock implementation
$(STRESSMOD)-objs += locks/aqs.o # Our implementation
$(STRESSMOD)-objs += locks/aqm.o # Our implementation
$(STRESSMOD)-objs += locks/cmcsmcs.o # Cohort MCS-MCS
$(STRESSMOD)-objs += locks/ctasmcs.o # Cohort TAS-MCS

$(STRESSMOD)-objs += locks/rwaqs_ntrl.o
$(STRESSMOD)-objs += locks/rwaqs_rp.o
$(STRESSMOD)-objs += locks/rwaqs_rp_v1.o
$(STRESSMOD)-objs += locks/cmcsmcsrw.o
$(STRESSMOD)-objs += locks/cna.o
$(STRESSMOD)-objs += locks/rwaqm.o
# $(STRESSMOD)-objs += locks/rwaqmevlv.o

ccflags-y := -DKERNEL_SYNCSTRESS -I$(src)/include/ -I$(src)/spinlock/

Q	= @

all: CPUSEQ $(BDIR_MAKEFILE)
	make -C $(KDIR) M=$(BDIR) src=$(CURDIR) modules

CPUSEQ:
	$(Q)./script/gen-cpuseq.py

$(BDIR_MAKEFILE): $(BDIR)
	$(Q)touch $(BDIR_MAKEFILE)

$(BDIR):
	$(Q)mkdir -p $(BDIR)/locks

clean:
	$(Q)rm -rf build
	$(Q)rm -f include/cpuseq.h
