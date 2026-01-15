CONFIG_FILE := $(if $(wildcard ../.config),../.config,../.config.default)
CONFIG_DEBUG_MODE_VAL := $(shell [ -f "$(CONFIG_FILE)" ] && grep '^DEBUG_MODE=' "$(CONFIG_FILE)" 2>/dev/null | cut -d= -f2- | sed 's/^[[:space:]]*//;s/[[:space:]]*$$//;s/"//g' | tr '[:upper:]' '[:lower:]' || echo "")
CONFIG_DEBUG_MODE := $(if $(filter y yes 1 true enable enabled,$(CONFIG_DEBUG_MODE_VAL)),1,0)
CONFIG_DEBUG_VERBOSITY_VAL := $(shell [ -f "$(CONFIG_FILE)" ] && grep '^DEBUG_VERBOSITY=' "$(CONFIG_FILE)" 2>/dev/null | cut -d= -f2- | sed 's/^[[:space:]]*//;s/[[:space:]]*$$//;s/"//g' || echo "")
CONFIG_DEBUG_VERBOSITY := $(if $(CONFIG_DEBUG_VERBOSITY_VAL),$(CONFIG_DEBUG_VERBOSITY_VAL),3)

CPPFLAGS:=$(CPPFLAGS) -DCONFIG_DEBUG_MODE=$(CONFIG_DEBUG_MODE) -DCONFIG_DEBUG_VERBOSITY=$(CONFIG_DEBUG_VERBOSITY)
