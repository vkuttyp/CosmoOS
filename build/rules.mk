# Generic compile rules. Objects mirror the source tree under $(OUT).
#
# Component makefiles call:
#   $(eval $(call compile_rules,<objs>,<cflags-variable-name>))
# which defines pattern rules for those objects with the given flags, plus
# a matching structured static-analysis report rule.

# Analyze every source on every invocation: timestamps alone do not track
# compiler/flag changes, and an incremental stdout-only gate loses reports.
.PHONY: analyze-force
analyze-force:

# $(1) = list of object paths under $(OUT)
# $(2) = name of the CFLAGS variable to use
# An object whose source is generated from Nova is compiled from $(NOVA_GEN) (build/nova.mk); every object waits
# for the generation, since any of them may include a generated header.
define compile_rules
$(call compile_from,$(filter-out $(NOVA_GEN_OBJS),$(1)),$(2),$(ROOT))
$(call compile_from,$(filter $(NOVA_GEN_OBJS),$(1)),$(2),$(NOVA_GEN))
endef

# $(1) = objects, $(2) = the CFLAGS variable's name, $(3) = the directory their sources are under
define compile_from
$(if $(strip $(1)),$(call compile_from_rules,$(1),$(2),$(3)))
endef

define compile_from_rules
$(1): $(OUT)/%.o: $(3)/%.c | $(NOVA_STAMP)
	$$(call log,CC,$$<)
	$$(Q)mkdir -p $$(dir $$@)
	$$(Q)$$(CC) $$($(2)) $$(EXTRA_CFLAGS) -MMD -MP -c $$< -o $$@

$(patsubst %.o,%.analyzed,$(1)): $(OUT)/%.analyzed: $(3)/%.c analyze-force | $(NOVA_STAMP)
	$$(call log,ANALYZE,$$<)
	$$(Q)mkdir -p $$(dir $$@)
	$$(Q)$$(CC) $$($(2)) $$(EXTRA_CFLAGS) --analyze -Xanalyzer -analyzer-output=plist-multi-file $$< -o $$@.tmp
	$$(Q)mv $$@.tmp $$@
endef

# Same for assembly. Assembly sources are preprocessed (.S).
define assemble_rules
$(1): $(OUT)/%.o: $(ROOT)/%.S | $(NOVA_STAMP)
	$$(call log,AS,$$<)
	$$(Q)mkdir -p $$(dir $$@)
	$$(Q)$$(CC) $$($(2)) $$(EXTRA_CFLAGS) -MMD -MP -c $$< -o $$@
endef

# Objects for a list of sources relative to $(ROOT).
objs_of = $(addprefix $(OUT)/,$(patsubst %.S,%.o,$(patsubst %.c,%.o,$(1))))
