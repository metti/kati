SUBMAKE ?= 0

ifneq ($(SUBMAKE),1)
export VAR
VAR = foo

sub:
	@echo VAR=$(VAR) SUBMAKE=$(SUBMAKE)
	$(MAKE) -f export_into_submake.mk SUBMAKE=1 test

else

test:
	@echo VAR=$(VAR) SUBMAKE=$(SUBMAKE)
endif
