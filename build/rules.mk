# Generic compile rules. Objects mirror the source tree under $(OUT).
#
# Component makefiles call:
#   $(eval $(call compile_rules,<objs>,<cflags-variable-name>))
# which defines pattern rules for those objects with the given flags, plus
# a matching static-analysis stamp rule.

# $(1) = list of object paths under $(OUT)
# $(2) = name of the CFLAGS variable to use
define compile_rules
$(1): $(OUT)/%.o: $(ROOT)/%.c
	$$(call log,CC,$$<)
	$$(Q)mkdir -p $$(dir $$@)
	$$(Q)$$(CC) $$($(2)) $$(EXTRA_CFLAGS) -MMD -MP -c $$< -o $$@

$(patsubst %.o,%.analyzed,$(1)): $(OUT)/%.analyzed: $(ROOT)/%.c
	$$(call log,ANALYZE,$$<)
	$$(Q)mkdir -p $$(dir $$@)
	$$(Q)$$(CC) $$($(2)) $$(EXTRA_CFLAGS) --analyze -Xanalyzer -analyzer-output=text $$< -o /dev/null
	$$(Q)touch $$@
endef

# Same for assembly. Assembly sources are preprocessed (.S).
define assemble_rules
$(1): $(OUT)/%.o: $(ROOT)/%.S
	$$(call log,AS,$$<)
	$$(Q)mkdir -p $$(dir $$@)
	$$(Q)$$(CC) $$($(2)) $$(EXTRA_CFLAGS) -MMD -MP -c $$< -o $$@
endef

# Objects for a list of sources relative to $(ROOT).
objs_of = $(addprefix $(OUT)/,$(patsubst %.S,%.o,$(patsubst %.c,%.o,$(1))))
