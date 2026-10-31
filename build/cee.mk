# The C generated from the kernel's CeeSafe sources (docs/build/design.md, "CeeSafe sources"; roadmap 2.0's CeeSafe
# rule, point 7). The tree holds the .cee files only: the build runs the pinned ceesafec (scripts/cee-gen.py, CEESAFEC
# or ceesafec on PATH) into $(CEE_GEN), and every object, host test and fuzz target made from a generated file takes it
# from there. A `make dist` tarball (.cee-dist at its root) carries the generated C beside each source instead, and
# builds with no ceesafec (CeeSafe's ADR 0023).
#
# CEE_GEN_FILES  every generated file, relative to the tree (cee-gen.py --list; no ceesafec needed)
# CEE_GEN        where they are: out/cee, or the tree itself in a dist tarball
# CEE_STAMP      the generation, an order-only prerequisite of every C object (empty in a dist tarball)
# CEE_CFLAGS     the generated headers' include path
# CEE_PREFIX_MAP the prefix map that gives the generated C the paths it has in a tarball, so a build from out/cee
#                 and one from a tarball are the same bytes
# src_paths       a list of sources relative to the tree, as paths: generated ones under CEE_GEN

PYTHON ?= python3

CEE_GEN_FILES := $(shell $(PYTHON) $(ROOT)/scripts/cee-gen.py --list)

ifneq ($(wildcard $(ROOT)/.cee-dist),)
CEE_GEN := $(ROOT)
CEE_STAMP :=
else
CEE_GEN := $(ROOT)/out/cee
CEE_STAMP := $(CEE_GEN)/.stamp
CEE_SOURCES := $(shell find $(ROOT)/kernel $(ROOT)/kernel-services $(ROOT)/drivers -name '*.cee' 2>/dev/null)

$(CEE_STAMP): $(CEE_SOURCES) $(ROOT)/scripts/cee-gen.py
	$(call log,CEE,$(CEE_GEN))
	$(Q)$(PYTHON) $(ROOT)/scripts/cee-gen.py --out out/cee --no-negative
	$(Q)touch $@

# Made by the generation; an empty recipe, so what depends on one is remade after it.
$(addprefix $(CEE_GEN)/,$(CEE_GEN_FILES)): $(CEE_STAMP) ;

# An object built while the generated C was in the tree has a dependency file (-MMD) naming it there. Those paths no
# longer exist; a rule with nothing to do lets make treat them as remade, so such an object is rebuilt from here.
$(addprefix $(ROOT)/,$(CEE_GEN_FILES)):
endif

CEE_CFLAGS := -I$(CEE_GEN)/kernel/include
# out/cee is inside the tree, so its map and the tree's own (toolchain.mk) both match a generated file's path, and
# which one wins has differed between clangs (the first given; the last). COMMON_CFLAGS gives this one on both sides
# of the tree's, so the generated file is /cosmo/<its path in the tree> either way.
CEE_PREFIX_MAP := $(if $(filter $(ROOT),$(CEE_GEN)),,-ffile-prefix-map=$(CEE_GEN)=/cosmo)
CEE_GEN_OBJS := $(addprefix $(OUT)/,$(patsubst %.c,%.o,$(filter %.c,$(CEE_GEN_FILES))))

src_paths = $(foreach s,$(1),$(if $(filter $(s),$(CEE_GEN_FILES)),$(CEE_GEN)/$(s),$(ROOT)/$(s)))
