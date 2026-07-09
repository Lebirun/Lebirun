CONFIG_FILE := $(if $(wildcard ../.config),../.config,../.config.default)

define read_bool
$(if $(filter y yes 1 true enable enabled,$(shell if [ -f "$(CONFIG_FILE)" ] && grep -q '^$(1)=' "$(CONFIG_FILE)" 2>/dev/null; then grep '^$(1)=' "$(CONFIG_FILE)" 2>/dev/null | tail -n 1 | cut -d= -f2; elif [ -f "../.config.default" ]; then grep '^$(1)=' "../.config.default" 2>/dev/null | tail -n 1 | cut -d= -f2; fi)),1,0)
endef

CONFIG_VIRT_VFL := $(call read_bool,VIRT_VFL)
CONFIG_DRIVER_AHCI := $(call read_bool,DRIVER_AHCI)
CONFIG_DRIVER_PS2_KEYBOARD := $(call read_bool,DRIVER_PS2_KEYBOARD)
CONFIG_DRIVER_PS2_MOUSE := $(call read_bool,DRIVER_PS2_MOUSE)
CONFIG_DRIVER_NET := $(call read_bool,DRIVER_NET)
CONFIG_DRIVER_NET_E1000 := $(call read_bool,DRIVER_NET_E1000)
CONFIG_MODULE_IPV67 := $(call read_bool,MODULE_IPV67)

CPPFLAGS:=$(CPPFLAGS) \
	-DCONFIG_VIRT_VFL=$(CONFIG_VIRT_VFL) \
	-DCONFIG_DRIVER_AHCI=$(CONFIG_DRIVER_AHCI) \
	-DCONFIG_DRIVER_PS2_KEYBOARD=$(CONFIG_DRIVER_PS2_KEYBOARD) \
	-DCONFIG_DRIVER_PS2_MOUSE=$(CONFIG_DRIVER_PS2_MOUSE) \
	-DCONFIG_DRIVER_NET=$(CONFIG_DRIVER_NET) \
	-DCONFIG_DRIVER_NET_E1000=$(CONFIG_DRIVER_NET_E1000) \
	-DCONFIG_MODULE_IPV67=$(CONFIG_MODULE_IPV67)
