# The C generated from the kernel's Nova sources (docs/build/design.md, "Nova sources"; roadmap 2.0's Nova
# rule, point 7). The tree holds the .nova files only: the build runs the pinned novac (scripts/nova-gen.py, NOVAC
# or novac on PATH) into $(NOVA_GEN), and every object, host test and fuzz target made from a generated file takes it
# from there. A `make dist` tarball (.nova-dist at its root) carries the generated C beside each source instead, and
# builds with no novac (Nova's ADR 0023).
#
# NOVA_GEN_FILES  every generated file, relative to the tree (nova-gen.py --list; no novac needed)
# NOVA_GEN        where they are: out/nova, or the tree itself in a dist tarball
# NOVA_STAMP      the generation, an order-only prerequisite of every C object (empty in a dist tarball)
# NOVA_CFLAGS     the generated headers' include path
# NOVA_PREFIX_MAP the prefix map that gives the generated C the paths it has in a tarball, so a build from out/nova
#                 and one from a tarball are the same bytes
# src_paths       a list of sources relative to the tree, as paths: generated ones under NOVA_GEN

PYTHON ?= python3

NOVA_GEN_FILES := $(shell $(PYTHON) $(ROOT)/scripts/nova-gen.py --list)

ifneq ($(wildcard $(ROOT)/.nova-dist),)
NOVA_GEN := $(ROOT)
NOVA_STAMP :=
else
NOVA_GEN := $(ROOT)/out/nova
NOVA_STAMP := $(NOVA_GEN)/.stamp
NOVA_SOURCES := $(shell find $(ROOT)/kernel $(ROOT)/kernel-services $(ROOT)/drivers -name '*.nova' 2>/dev/null)

$(NOVA_STAMP): $(NOVA_SOURCES) $(ROOT)/scripts/nova-gen.py
	$(call log,NOVA,$(NOVA_GEN))
	$(Q)$(PYTHON) $(ROOT)/scripts/nova-gen.py --out out/nova --no-negative
	$(Q)touch $@

# Made by the generation; an empty recipe, so what depends on one is remade after it.
$(addprefix $(NOVA_GEN)/,$(NOVA_GEN_FILES)): $(NOVA_STAMP) ;

# An object built while the generated C was in the tree has a dependency file (-MMD) naming it there. Those paths no
# longer exist; a rule with nothing to do lets make treat them as remade, so such an object is rebuilt from here.
$(addprefix $(ROOT)/,$(NOVA_GEN_FILES)):
endif

NOVA_CFLAGS := -I$(NOVA_GEN)/kernel/include
# out/nova is inside the tree, so its map and the tree's own (toolchain.mk) both match a generated file's path, and
# which one wins has differed between clangs (the first given; the last). COMMON_CFLAGS gives this one on both sides
# of the tree's, so the generated file is /cosmo/<its path in the tree> either way.
NOVA_PREFIX_MAP := $(if $(filter $(ROOT),$(NOVA_GEN)),,-ffile-prefix-map=$(NOVA_GEN)=/cosmo)
NOVA_GEN_OBJS := $(addprefix $(OUT)/,$(patsubst %.c,%.o,$(filter %.c,$(NOVA_GEN_FILES))))

src_paths = $(foreach s,$(1),$(if $(filter $(s),$(NOVA_GEN_FILES)),$(NOVA_GEN)/$(s),$(ROOT)/$(s)))
